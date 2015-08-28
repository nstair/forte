/*
 *  fci_davidson_liu.cc
 *  Capriccio
 *
 *  Created by Francesco Evangelista on 3/21/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#include <cmath>
#include <numeric>

#include <boost/timer.hpp>

#include <libciomr/libciomr.h>
#include <liboptions/liboptions.h>
#include <libmoinfo/libmoinfo.h>
#include <libmints/molecule.h>
#include <libpsio/psio.h>
#include <libpsio/psio.hpp>
#include "libmints/matrix.h"
#include "libmints/vector.h"

#include "bitset_determinant.h"
#include "integrals.h"
#include "iterative_solvers.h"
#include "fci_solver.h"
#include "string_lists.h"
#include "helpers.h"
#include "reference.h"

#include <psi4-dec.h>

using namespace std;
using namespace psi;
using namespace boost;

extern double h1_aa_timer;
extern double h1_bb_timer;
extern double h2_aaaa_timer;
extern double h2_aabb_timer;
extern double h2_bbbb_timer;
extern double oo_list_timer;
extern double vo_list_timer;
extern double vovo_list_timer;
extern double vvoo_list_timer;

int fci_debug_level = 4;

namespace psi{ namespace libadaptive{

class MOSpaceInfo;

FCI::FCI(boost::shared_ptr<Wavefunction> wfn, Options &options,
         ExplorerIntegrals* ints, std::shared_ptr<MOSpaceInfo> mo_space_info)
    : Wavefunction(options,_default_psio_lib_),
      options_(options), ints_(ints), mo_space_info_(mo_space_info)
{
    // Copy the wavefunction information
    copy(wfn);

    startup();
}

FCI::~FCI()
{
    if (fcisolver_ != nullptr) delete fcisolver_;
}

void FCI::startup()
{  
    print_method_banner({"String-based Full Configuration Interaction","by Francesco A. Evangelista"});
}

double FCI::compute_energy()
{
    double nuclear_repulsion_energy = molecule_->nuclear_repulsion_energy();

    Dimension active_dim = mo_space_info_->get_dimension("ACTIVE");
    size_t nfdocc = mo_space_info_->size("FROZEN_DOCC");
    std::vector<size_t> rdocc = mo_space_info_->get_corr_abs_mo("RESTRICTED_DOCC");
    std::vector<size_t> active = mo_space_info_->get_corr_abs_mo("ACTIVE");

    int charge       = Process::environment.molecule()->molecular_charge();
    if(options_["CHARGE"].has_changed()){
        charge = options_.get_int("CHARGE");
    }

    int nel = 0;
    int natom = Process::environment.molecule()->natom();
    for(int i=0; i < natom;i++){
        nel += static_cast<int>(Process::environment.molecule()->Z(i));
    }
    // If the charge has changed, recompute the number of electrons
    // Or if you cannot find the number of electrons
    nel -= charge;

    int multiplicity = Process::environment.molecule()->multiplicity();
    if(options_["MULTIPLICITY"].has_changed()){
        multiplicity = options_.get_int("MULTIPLICITY");
    }

    int ms = multiplicity - 1;
    if(options_["MS"].has_changed()){
        ms = options_.get_int("MS");
    }

    if(ms < 0){
        outfile->Printf("\n  Ms must be no less than 0.");
        outfile->Printf("\n  Ms = %2d, MULTI = %2d", ms, multiplicity);
        outfile->Printf("\n  Check (specify) Ms value (component of multiplicity)! \n");
        throw PSIEXCEPTION("Ms must be no less than 0. Check output for details.");
    }

    outfile->Printf("\n  Number of electrons: %d",nel);
    outfile->Printf("\n  Charge: %d",charge);
    outfile->Printf("\n  Multiplicity: %d",multiplicity);
    if (ms % 2 == 0){
        outfile->Printf("\n  M_s: %d",ms / 2);
    }else{
        outfile->Printf("\n  M_s: %d/2",ms);
    }

    if( ((nel - ms) % 2) != 0)
        throw PSIEXCEPTION("\n\n  FCI: Wrong value of M_s.\n\n");

    // Adjust the number of for frozen and restricted doubly occupied
    size_t nactel = nel - 2 * nfdocc - 2 * rdocc.size();

    size_t na = (nactel + ms) / 2;
    size_t nb =  nactel - na;

    //    size_t na = doccpi_.sum() + soccpi_.sum() - nfdocc - rdocc.size();
    //    size_t nb = doccpi_.sum() - nfdocc - rdocc.size();

    fcisolver_ = new FCISolver(active_dim,rdocc,active,na,nb,multiplicity,options_.get_int("ROOT_SYM"),ints_);


    fcisolver_->test_rdms(options_.get_bool("TEST_RDMS"));
    fcisolver_->set_print(options_.get_int("PRINT"));

    double fci_energy = fcisolver_->compute_energy();


    Process::environment.globals["CURRENT ENERGY"] = fci_energy;
    Process::environment.globals["FCI ENERGY"] = fci_energy;

    return fci_energy;
}

Reference FCI::reference()
{
    return fcisolver_->reference();
}


FCISolver::FCISolver(Dimension active_dim,std::vector<size_t> core_mo,
                     std::vector<size_t> active_mo,
                     size_t na, size_t nb, size_t multiplicity, size_t symmetry,
                     ExplorerIntegrals* ints)
    : active_dim_(active_dim), core_mo_(core_mo), active_mo_(active_mo),
      ints_(ints), nirrep_(active_dim.n()), symmetry_(symmetry),
      na_(na), nb_(nb), multiplicity_(multiplicity), nroot_(0)
{
    startup();
}

void FCISolver::startup()
{
    // Create the string lists
    lists_ = std::shared_ptr<StringLists>(new StringLists(twoSubstituitionVVOO,active_dim_,core_mo_,active_mo_,na_,nb_));

    size_t ndfci = 0;
    for (int h = 0; h < nirrep_; ++h){
        size_t nastr = lists_->alfa_graph()->strpi(h);
        size_t nbstr = lists_->beta_graph()->strpi(h ^ symmetry_);
        ndfci += nastr * nbstr;
    }
    outfile->Printf("\n\n  ==> FCI Solver <==\n");
    outfile->Printf("\n  Number of determinants    = %zu",ndfci);

}

/*
 * See Appendix A in J. Comput. Chem. 2001 vol. 22 (13) pp. 1574-1589
*/
double FCISolver::compute_energy()
{
    boost::timer t;

    double nuclear_repulsion_energy = Process::environment.molecule()->nuclear_repulsion_energy();

    std::shared_ptr<FCIIntegrals> fci_ints = std::make_shared<FCIIntegrals>(lists_,ints_);

    FCIWfn::allocate_temp_space(lists_,symmetry_);

    nroot_ = 1;

    FCIWfn Hdiag(lists_,symmetry_);
    C_ = std::make_shared<FCIWfn>(lists_,symmetry_);
    FCIWfn HC(lists_,symmetry_);

    size_t fci_size = Hdiag.size();
    Hdiag.form_H_diagonal(fci_ints);

    SharedVector b(new Vector("b",fci_size));
    SharedVector sigma(new Vector("sigma",fci_size));

    Hdiag.copy_to(sigma);


    DavidsonLiuSolver dls(fci_size,nroot_);
    dls.set_e_convergence(1.0e-13);
    dls.set_print_level(0);
    dls.startup(sigma);

    size_t guess_size = dls.collapse_size();
    auto guess = initial_guess(Hdiag,guess_size,multiplicity_);

    guess_size = std::min(guess.size(),guess_size);
    if (guess_size == 0){
        throw PSIEXCEPTION("\n\n  Found zero FCI guesses with the requested multiplicity.\n\n");
    }
    for (size_t n = 0; n < guess_size; ++n){
        HC.set(guess[n]);
        HC.copy_to(sigma);
        dls.add_b(sigma);
    }


    bool converged = false;

    outfile->Printf("\n\n  ==> Diagonalizing Hamiltonian <==\n");
    outfile->Printf("\n  ----------------------------------------");
    outfile->Printf("\n    Iter.      Avg. Energy       Delta_E");
    outfile->Printf("\n  ----------------------------------------");

    double old_avg_energy = 0.0;
    for (int cycle = 0; cycle < 30; ++cycle){
        bool add_sigma = true;
        for (int r = 0; r < nroot_ * 10; ++r){ // TODO : fix this loop
            dls.get_b(b);
            C_->copy(b);
            C_->Hamiltonian(HC,fci_ints,twoSubstituitionVVOO);
            HC.copy_to(sigma);
            add_sigma = dls.add_sigma(sigma);
            if (not add_sigma) break;
        }
        converged = dls.update();

        double avg_energy = 0.0;
        for (int r = 0; r < nroot_; ++r){
            avg_energy += dls.eigenvalues()->get(0) + nuclear_repulsion_energy;
        }
        avg_energy /= static_cast<double>(nroot_);

        outfile->Printf("\n    %3d  %20.12f  %+.3e",cycle,avg_energy,avg_energy - old_avg_energy);
        old_avg_energy = avg_energy;

        if (converged) break;
    }

    outfile->Printf("\n  ----------------------------------------");

    converged = true;

    if (converged){
        dls.get_results();
    }

    for (int r = 0; r < nroot_; ++r){ // TODO : fix this loop
        outfile->Printf("\n\n  ==> Root No. %d <==",r);
        double root_energy = dls.eigenvalues()->get(r) + nuclear_repulsion_energy;
//        double root_energy = dls.eigenvalues()->get(r) + nuclear_repulsion_energy + ints_->scalar();
        outfile->Printf("\n    Total Energy: %25.15f",root_energy);
    }

    // Compute the RDMs
    size_t rdm_root = 0;
    if (converged){
        C_->copy(dls.eigenvector(0));
        outfile->Printf("\n\n  ==> RDMs for Root No. %d <==",rdm_root);
        C_->compute_rdms(3);

        // Optionally, test the RDMs
        if (test_rdms_) C_->rdm_test();
    }

    energy_ = dls.eigenvalues()->get(0) + nuclear_repulsion_energy;

    return energy_;
}

std::vector<std::vector<std::tuple<size_t,size_t,size_t,double>>> FCISolver::initial_guess(FCIWfn& diag,size_t n,size_t multiplicity)
{
    boost::timer t;

    double nuclear_repulsion_energy = Process::environment.molecule()->nuclear_repulsion_energy();

    size_t ntrial = n * ntrial_per_root_;

    // Get the list of most important determinants
    std::vector<std::tuple<double,size_t,size_t,size_t>> dets = diag.get_largest_contributions(ntrial);

    size_t num_dets = dets.size();

    std::vector<BitsetDeterminant> bsdets;

    // Build the full determinants
    size_t nact = active_mo_.size();
    size_t nmo = ints_->ncmo();

    for (auto det : dets){
        double e;
        size_t h, add_Ia, add_Ib;
        std::tie(e,h,add_Ia,add_Ib) = det;
        boost::dynamic_bitset<> Ia_v = lists_->alfa_str(h,add_Ia);
        boost::dynamic_bitset<> Ib_v = lists_->beta_str(h ^ symmetry_,add_Ib);

        std::vector<bool> Ia(nmo,false);
        std::vector<bool> Ib(nmo,false);

        for (size_t i : core_mo_){
            Ia[i] = true;
            Ib[i] = true;
        }
        for (size_t i = 0; i < nact; ++i){
            if (Ia_v[i]) Ia[active_mo_[i]] = true;
            if (Ib_v[i]) Ib[active_mo_[i]] = true;
        }
        BitsetDeterminant bsdet(Ia,Ib);
        bsdets.push_back(bsdet);
    }

    Matrix H("H",num_dets,num_dets);
    Matrix evecs("Evecs",num_dets,num_dets);
    Vector evals("Evals",num_dets);

    for (size_t I = 0; I < num_dets; ++I){
        for (size_t J = I; J < num_dets; ++J){
            double HIJ = bsdets[I].slater_rules(bsdets[J]);
            H.set(I,J,HIJ);
            H.set(J,I,HIJ);
        }
    }

    H.diagonalize(evecs,evals);

    std::vector<std::vector<std::tuple<size_t,size_t,size_t,double>>> guess;

    print_h2("FCI Initial Guess");
    outfile->Printf("\n  ---------------------------------------------");
    outfile->Printf("\n    Root            Energy     <S^2>   Spin");
    outfile->Printf("\n  ---------------------------------------------");
    std::vector<string> s2_labels({"singlet","doublet","triplet","quartet","quintet","sextet","septet","octet","nonet","decaet","11-et","12-et"});

    for (size_t r = 0; r < num_dets; ++r){
        double energy = evals.get(r) + nuclear_repulsion_energy;
        double norm = 0.0;
        double S2 = 0.0;
        for (int I = 0; I < num_dets; ++I){
            for (int J = 0; J < num_dets; ++J){
                const double S2IJ = bsdets[I].spin2(bsdets[J]);
                S2 += evecs.get(I,r) * evecs.get(J,r) * S2IJ;
            }
            norm += std::pow(evecs.get(I,r),2.0);
        }
        S2 /= norm;
        double S = std::fabs(0.5 * (std::sqrt(1.0 + 4.0 * S2) - 1.0));
        int SS = std::round(S * 2.0);
        size_t state_multp = SS + 1;
        std::string state_label = s2_labels[SS];
        outfile->Printf("\n    %3d  %20.12f  %.3f  %s",r,energy,std::fabs(S2),state_label.c_str());
        // Save states of the desired multiplicity
        if (state_multp == multiplicity){
            std::vector<std::tuple<size_t,size_t,size_t,double>> solution;
            for (int I = 0; I < num_dets; ++I){
                auto det = dets[I];
                double e;
                size_t h, add_Ia, add_Ib;
                std::tie(e,h,add_Ia,add_Ib) = det;
                solution.push_back(std::make_tuple(h,add_Ia,add_Ib,evecs.get(I,r)));
            }
            guess.push_back(solution);
        }
    }
    outfile->Printf("\n  ---------------------------------------------");
    outfile->Printf("\n  Timing for initial guess  = %10.3f s\n",t.elapsed());
    outfile->Flush();

    return guess;
}

Reference FCISolver::reference()
{
    size_t nact = active_dim_.sum();
    size_t nact2 = nact * nact;
    size_t nact3 = nact2 * nact;
    size_t nact4 = nact3 * nact;
    size_t nact5 = nact4 * nact;

    // One-particle density matrices in the active space
    std::vector<double>& opdm_a = C_->opdm_a();
    std::vector<double>& opdm_b = C_->opdm_b();
    ambit::Tensor L1a = ambit::Tensor::build(ambit::kCore,"L1a",{nact,nact});
    ambit::Tensor L1b = ambit::Tensor::build(ambit::kCore,"L1b",{nact,nact});
    L1a.iterate([&](const::vector<size_t>& i,double& value){
        value = opdm_a[i[0] * nact + i[1]]; });
    L1b.iterate([&](const::vector<size_t>& i,double& value){
        value = opdm_b[i[0] * nact + i[1]]; });

    // Two-particle density matrices in the active space
    ambit::Tensor L2aa = ambit::Tensor::build(ambit::kCore,"L2aa",{nact,nact,nact,nact});
    ambit::Tensor L2ab = ambit::Tensor::build(ambit::kCore,"L2ab",{nact,nact,nact,nact});
    ambit::Tensor L2bb = ambit::Tensor::build(ambit::kCore,"L2bb",{nact,nact,nact,nact});

    if (na_ >= 2){
        std::vector<double>& tpdm_aa = C_->tpdm_aa();
        L2aa.iterate([&](const::vector<size_t>& i,double& value){
            value = tpdm_aa[i[0] * nact3 + i[1] * nact2 + i[2] * nact + i[3]]; });
    }
    if ((na_ >= 1) and (nb_ >= 1)){
        std::vector<double>& tpdm_ab = C_->tpdm_ab();
        L2ab.iterate([&](const::vector<size_t>& i,double& value){
            value = tpdm_ab[i[0] * nact3 + i[1] * nact2 + i[2] * nact + i[3]]; });
    }
    if (nb_ >= 2){
        std::vector<double>& tpdm_bb = C_->tpdm_bb();
        L2bb.iterate([&](const::vector<size_t>& i,double& value){
            value = tpdm_bb[i[0] * nact3 + i[1] * nact2 + i[2] * nact + i[3]]; });
    }

    // Convert the 2-RDMs to 2-RCMs
    L2aa("pqrs") -= L1a("pr") * L1a("qs");
    L2aa("pqrs") += L1a("ps") * L1a("qr");

    L2ab("pqrs") -= L1a("pr") * L1a("qs");

    L2bb("pqrs") -= L1b("pr") * L1b("qs");
    L2bb("pqrs") += L1b("ps") * L1b("qr");

    // Three-particle density matrices in the active space
    ambit::Tensor L3aaa = ambit::Tensor::build(ambit::kCore,"L3aaa",{nact,nact,nact,nact,nact,nact});
    ambit::Tensor L3aab = ambit::Tensor::build(ambit::kCore,"L3aab",{nact,nact,nact,nact,nact,nact});
    ambit::Tensor L3abb = ambit::Tensor::build(ambit::kCore,"L3abb",{nact,nact,nact,nact,nact,nact});
    ambit::Tensor L3bbb = ambit::Tensor::build(ambit::kCore,"L3bbb",{nact,nact,nact,nact,nact,nact});
    if (na_ >= 3){
        std::vector<double>& tpdm_aaa = C_->tpdm_aaa();
        L3aaa.iterate([&](const::vector<size_t>& i,double& value){
            value = tpdm_aaa[i[0] * nact5 + i[1] * nact4 + i[2] * nact3 + i[3] * nact2 + i[4] * nact + i[5]]; });
    }
    if ((na_ >= 2) and (nb_ >= 1)){
        std::vector<double>& tpdm_aab = C_->tpdm_aab();
        L3aab.iterate([&](const::vector<size_t>& i,double& value){
            value = tpdm_aab[i[0] * nact5 + i[1] * nact4 + i[2] * nact3 + i[3] * nact2 + i[4] * nact + i[5]]; });
    }
    if ((na_ >= 1) and (nb_ >= 2)){
        std::vector<double>& tpdm_abb = C_->tpdm_abb();
        L3abb.iterate([&](const::vector<size_t>& i,double& value){
            value = tpdm_abb[i[0] * nact5 + i[1] * nact4 + i[2] * nact3 + i[3] * nact2 + i[4] * nact + i[5]]; });
    }
    if (nb_ >= 3){
        std::vector<double>& tpdm_bbb = C_->tpdm_bbb();
        L3bbb.iterate([&](const::vector<size_t>& i,double& value){
            value = tpdm_bbb[i[0] * nact5 + i[1] * nact4 + i[2] * nact3 + i[3] * nact2 + i[4] * nact + i[5]]; });
    }

    // Convert the 3-RDMs to 3-RCMs
    L3aaa("pqrstu") -= L1a("ps") * L2aa("qrtu");
    L3aaa("pqrstu") += L1a("pt") * L2aa("qrsu");
    L3aaa("pqrstu") += L1a("pu") * L2aa("qrts");

    L3aaa("pqrstu") -= L1a("qt") * L2aa("prsu");
    L3aaa("pqrstu") += L1a("qs") * L2aa("prtu");
    L3aaa("pqrstu") += L1a("qu") * L2aa("prst");

    L3aaa("pqrstu") -= L1a("ru") * L2aa("pqst");
    L3aaa("pqrstu") += L1a("rs") * L2aa("pqut");
    L3aaa("pqrstu") += L1a("rt") * L2aa("pqsu");

    L3aaa("pqrstu") -= L1a("ps") * L1a("qt") * L1a("ru");
    L3aaa("pqrstu") -= L1a("pt") * L1a("qu") * L1a("rs");
    L3aaa("pqrstu") -= L1a("pu") * L1a("qs") * L1a("rt");

    L3aaa("pqrstu") += L1a("ps") * L1a("qu") * L1a("rt");
    L3aaa("pqrstu") += L1a("pu") * L1a("qt") * L1a("rs");
    L3aaa("pqrstu") += L1a("pt") * L1a("qs") * L1a("ru");


    L3aab("pqRstU") -= L1a("ps") * L2ab("qRtU");
    L3aab("pqRstU") += L1a("pt") * L2ab("qRsU");

    L3aab("pqRstU") -= L1a("qt") * L2ab("pRsU");
    L3aab("pqRstU") += L1a("qs") * L2ab("pRtU");

    L3aab("pqRstU") -= L1b("RU") * L2aa("pqst");

    L3aab("pqRstU") -= L1a("ps") * L1a("qt") * L1b("RU");
    L3aab("pqRstU") += L1a("pt") * L1a("qs") * L1b("RU");


    L3abb("pQRsTU") -= L1a("ps") * L2bb("QRTU");

    L3abb("pQRsTU") -= L1b("QT") * L2ab("pRsU");
    L3abb("pQRsTU") += L1b("QU") * L2ab("pRsT");

    L3abb("pQRsTU") -= L1b("RU") * L2ab("pQsT");
    L3abb("pQRsTU") += L1b("RT") * L2ab("pQsU");

    L3abb("pQRsTU") -= L1a("ps") * L1b("QT") * L1b("RU");
    L3abb("pQRsTU") += L1a("ps") * L1b("QU") * L1b("RT");


    L3bbb("pqrstu") -= L1b("ps") * L2bb("qrtu");
    L3bbb("pqrstu") += L1b("pt") * L2bb("qrsu");
    L3bbb("pqrstu") += L1b("pu") * L2bb("qrts");

    L3bbb("pqrstu") -= L1b("qt") * L2bb("prsu");
    L3bbb("pqrstu") += L1b("qs") * L2bb("prtu");
    L3bbb("pqrstu") += L1b("qu") * L2bb("prst");

    L3bbb("pqrstu") -= L1b("ru") * L2bb("pqst");
    L3bbb("pqrstu") += L1b("rs") * L2bb("pqut");
    L3bbb("pqrstu") += L1b("rt") * L2bb("pqsu");

    L3bbb("pqrstu") -= L1b("ps") * L1b("qt") * L1b("ru");
    L3bbb("pqrstu") -= L1b("pt") * L1b("qu") * L1b("rs");
    L3bbb("pqrstu") -= L1b("pu") * L1b("qs") * L1b("rt");

    L3bbb("pqrstu") += L1b("ps") * L1b("qu") * L1b("rt");
    L3bbb("pqrstu") += L1b("pu") * L1b("qt") * L1b("rs");
    L3bbb("pqrstu") += L1b("pt") * L1b("qs") * L1b("ru");

    if (print_ > 1)
    for (auto L1 : {L1a,L1b}){
        outfile->Printf("\n\n** %s **",L1.name().c_str());
        L1.iterate([&](const::vector<size_t>& i,double& value){
            if (std::fabs(value) > 1.0e-15)
                outfile->Printf("\n  Lambda [%3lu][%3lu] = %18.15lf", i[0], i[1], value);
        });

    }

    if (print_ > 2)
    for (auto L2 : {L2aa,L2ab,L2bb}){
        outfile->Printf("\n\n** %s **",L2.name().c_str());
        L2.iterate([&](const::vector<size_t>& i,double& value){
            if (std::fabs(value) > 1.0e-15)
                outfile->Printf("\n  Lambda [%3lu][%3lu][%3lu][%3lu] = %18.15lf", i[0], i[1], i[2], i[3], value);
        });

    }

    if (print_ > 3)
    for (auto L3 : {L3aaa,L3aab,L3abb,L3bbb}){
        outfile->Printf("\n\n** %s **",L3.name().c_str());
        L3.iterate([&](const::vector<size_t>& i,double& value){
            if (std::fabs(value) > 1.0e-15)
                outfile->Printf("\n  Lambda [%3lu][%3lu][%3lu][%3lu][%3lu][%3lu] = %18.15lf", i[0], i[1], i[2], i[3], i[4], i[5], value);
        });
    }

    Reference fci_ref(energy_,L1a,L1b,L2aa,L2ab,L2bb,L3aaa,L3aab,L3abb,L3bbb);
    return fci_ref;
}

}}