﻿/*
 * @BEGIN LICENSE
 *
 * Forte: an open-source plugin to Psi4 (https://github.com/psi4/psi4)
 * that implements a variety of quantum chemistry methods for strongly
 * correlated electrons.
 *
 * Copyright (c) 2012-2017 by its authors (see COPYING, COPYING.LESSER, AUTHORS).
 *
 * The copyrights for code used from other parties are included in
 * the corresponding files.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 *
 * @END LICENSE
 */

#include "psi4/libpsi4util/process.h"
#include "psi4/libmints/molecule.h"
#include "psi4/libmints/wavefunction.h"
#include "psi4/libmints/mintshelper.h"
#include "psi4/liboptions/liboptions.h"
#include "psi4/libfock/jk.h"
#include "math.h"
#include <string>
#include "embedding.h"
#include "orbital-helper/iao_builder.h"
#include "forte_options.h"

namespace psi {
namespace forte {

// using namespace ambit;

Embedding::Embedding(SharedWavefunction ref_wfn, Options& options,
                     std::shared_ptr<ForteIntegrals> ints,
                     std::shared_ptr<MOSpaceInfo> mo_space_info)
    : Wavefunction(options), ints_(ints), mo_space_info_(mo_space_info) {

    shallow_copy(ref_wfn);
    ref_wfn_ = ref_wfn;
    print_method_banner({"Embedded calculation A-in-B", "Nan He"});

    startup();
}

void set_EMBEDDING_options(ForteOptions& foptions) {
    foptions.add_str("LOCALIZATION_METHOD", "IAO", "Method to localize orbital: IAO or other");
    foptions.add_str("ORBITAL_SEPARATION", "IAOLABEL",
                     "Method of orbital separation: IAOLABEL, IAOCHARGE, LARGE_C");
    foptions.add_str("SYSTEM_MATRIX", "SYS", "Size of system Matrix: SYS, ALL");
    foptions.add_str("C_SIZE", "SYS", "Size of coeffs: SYS, ALL");
    foptions.add_int("SYS_DOCC", 0, "System occupancy");
}

void Embedding::startup() {}


SharedMatrix AO_TEI_all(SharedWavefunction wfn) {
	MintsHelper mints(wfn);
	SharedMatrix TEI = mints.ao_eri();
	return TEI;
}

double eri_index(SharedMatrix TEI, int u, int v, int l, int s, Dimension nmopi) {
	return TEI->get(u * nmopi[0] + v, l * nmopi[0] + s); // Currently works only for C1 !!!
}

void build_D(SharedMatrix C, Dimension noccpi, SharedMatrix D) {
    int nirrep = C->nirrep();
    Dimension nmopi = C->colspi();
    for (int h = 0; h < nirrep; ++h) {
        for (int u = 0; u < nmopi[h]; ++u) {
            for (int v = 0; v < nmopi[h]; ++v) {
                double tmp = 0.0;
                for (int m = 0; m < noccpi[h]; ++m) {
                    tmp += C->get(h, u, m) * C->get(h, v, m);
                }
                D->set(h, u, v, tmp);
            }
        }
    }
}

SharedMatrix build_Cocc(SharedMatrix C, int nirrep, Dimension nmopi, Dimension noccpi) {
    SharedMatrix C_occ(new Matrix("C_occ", nirrep, nmopi, nmopi)); // make it ready for
                                                                   // changes!!here
    for (int h = 0; h < nirrep; ++h) {
        for (int u = 0; u < nmopi[h]; ++u) {
            for (int i = 0; i < noccpi[h]; ++i) {
                C_occ->set(h, u, i, C->get(h, u, i));
            }
        }
    }
    return C_occ;
}


SharedMatrix build_Focc(SharedMatrix F, int nirrep, Dimension nmopi, Dimension noccpi) {
    SharedMatrix F_occ(new Matrix("F_occ", nirrep, nmopi, nmopi)); // make it ready for
                                                                   // changes!!here
    for (int h = 0; h < nirrep; ++h) {
        for (int u = 0; u < nmopi[h]; ++u) {
            for (int i = 0; i < noccpi[h]; ++i) {
                F_occ->set(h, u, i, F->get(h, u, i));
            }
        }
    }
    return F_occ;
}

SharedMatrix build_G_withD(SharedWavefunction wfn, SharedMatrix D) {
    SharedMatrix TEI = AO_TEI_all(wfn);
    Dimension nmopi = D->rowspi();
    int nirrep = D->nirrep();
    SharedMatrix G(new Matrix("G_uv", nirrep, nmopi, nmopi));
    // G->zero();
    double tmp = 0.0;
    double jkfactor = 0.0;
    for (int h = 0; h < wfn->nirrep(); ++h) { // This part doesn't work for symmetry higher than
                                              // C1!!
        for (int u = 0; u < nmopi[h]; ++u) {
            for (int v = 0; v < nmopi[h]; ++v) {
                tmp = 0.0;
                for (int l = 0; l < nmopi[h]; ++l) {
                    for (int s = 0; s < nmopi[h]; ++s) {
                        jkfactor = 2 * eri_index(TEI, u, v, l, s, nmopi) -
                                   eri_index(TEI, u, l, v, s, nmopi);
                        tmp += D->get(h, l, s) * jkfactor;
                    }
                }
                G->set(h, u, v, tmp);
            }
        }
    }
    return G;
}

SharedMatrix Separate_orbital_subset(SharedWavefunction wfn, int natom_sys,
                                     std::map<std::string, SharedMatrix> Loc, Options& options) {
    // Write separation code here soon
    outfile->Printf("\n *** Orbital assignment and separation *** \n");
    Dimension nmopi = wfn->nmopi();
    int nirrep = wfn->nirrep();
    std::shared_ptr<BasisSet> basis = wfn->basisset();

    // generate basis_sys for mol_sys later if we want different basis for sys and env!
    int nbf = basis->nbf();
    outfile->Printf("\n number of basis on all atoms: %d", nbf);
    // count how many basis on system atoms
    int count_basis = 0;
    for (int A = 0; A < natom_sys; A++) {
        int n_shell = basis->nshell_on_center(A);
        for (int Q = 0; Q < n_shell; Q++) {
            const GaussianShell& shell = basis->shell(A, Q);
            count_basis += shell.nfunction();
        }
    }
    outfile->Printf("\n number of basis on \"system\" atoms: %d", count_basis);

    // SharedMatrix C_sys(new Matrix("System Coeffs", nirrep, nbasispi, nbasispi));

    std::vector<int> index_trace = {};
    int count_orbs = 0;

    if (options.get_str("ORBITAL_SEPARATION") ==
        "LARGE_C") { // Simple method, assign orbitals according to largest C position
        // C can only be C1 due to IAO code, so ignore symmetry!
        outfile->Printf("\n Simple catagorization algorithm: largest C^2 \n");
        for (int i = 0; i < nmopi[0]; ++i) {
            outfile->Printf("\n Running over the %d th MO \n", i);
            // loop over C rows (i:nmopi[h]), find the largest value, return index
            SharedVector r = Loc["L_local"]->get_column(0, i);
            double tmp = 0.0;
            int index_this = 0;
            for (int j = 0; j < nmopi[0]; ++j) {
                if (fabs(r->get(j)) > tmp) {
                    index_this = j; // find the largest C index, record row number (AO index!)
                    tmp = r->get(0, j);
                }
            }
            if (index_this <
                count_basis) { // Change this if symmetry beyond c1 is allowed in the future
                // r belongs to system!
                index_trace.push_back(i); // record index of MO column
                outfile->Printf("\n find %d", i);
                ++count_orbs;
            }
            // r belongs to environment, do nothing
        }
    }

    if (options.get_str("ORBITAL_SEPARATION") ==
        "IAOCHARGE") { // IAO charge method based on partial charge
                       // C can only be C1 due to IAO code, so ignore symmetry!
        outfile->Printf("\n Charge-based catagorization algorithm \n");
        for (int i = 0; i < nmopi[0]; ++i) {
            outfile->Printf("\n Running over the %d th MO \n", i);
            // loop over C rows (i:nmopi[h]), find the largest value, return index
            SharedVector r = Loc["Q"]->get_column(0, i);
            double tmp = 0.0;
            int index_this = 0;
            for (int j = 0; j < nmopi[0]; ++j) {
                if (fabs(r->get(j)) > tmp) {
                    index_this = j; // find the largest C index, record row number (AO index!)
                    tmp = r->get(0, j);
                }
            }
            if (index_this < natom_sys) { // Change this if symmetry beyond c1 is allowed in the
                                          // future r belongs to system!
                index_trace.push_back(i); // record index of MO column
                outfile->Printf("\n find %d", i);
                ++count_orbs;
            }
            // r belongs to environment, do nothing
        }
    }

    if (options.get_str("ORBITAL_SEPARATION") == "IAOLABEL") { // Use IAO labels directly!
        int thres = natom_sys + 1;
        for (int i = 0; i < nmopi[0];
             ++i) { // Change this if symmetry beyond c1 is allowed in the future
            int tmp = Loc["I"]->get(0, i, i);
            if (tmp < thres) {
                index_trace.push_back(i);
                ++count_orbs;
            }
        }
    }

    // Use index_trace to generate C_sys
    Dimension nmo_sys_pi = nmopi;
    Dimension nbasis_sys_pi = nmopi;
    for (int h = 0; h < nirrep; ++h) {
        nmo_sys_pi[h] = count_orbs; // Change this if symmetry beyond c1 is allowed in the future
    }
    for (int h = 0; h < nirrep; ++h) {
        nbasis_sys_pi[h] = count_basis; // Change this if symmetry beyond c1 is allowed in the
                                        // future
    }
    // int nempty = nbf - nmo_sys_pi[0];
    SharedMatrix C_sys(new Matrix("System Coeffs", nirrep, nmopi, nmo_sys_pi));
    // add loops here if symmetry beyond c1 is allowed in the future
    for (int k = 0; k < count_orbs; ++k) {
        SharedVector r = Loc["IAO"]->get_column(0, index_trace[k]);
        outfile->Printf("\n ** the %dth IAO coeffs on \"system\" atom found, index is %d ** ", k,
                        index_trace[k]);
        r->print();

        /*
        //project or shrink r to small basis
        SharedVector r_short(new Vector("MO coeffs in small basis", nbasis_sys_pi));
        for (int l = 0; l < count_basis; ++l) {
                r_short->set(0, l, r->get(0, l));
        }
        */
        C_sys->set_column(0, k, r);
    }
    outfile->Printf("\n ** IAO coeffs on \"system\" atoms (AO basis) ** \n");
    C_sys->print();

    // SharedWavefunction wfn_A(new Wavefunction(mol_sys, wfn->basisset()));
    // SharedMatrix C_new = wfn_A->Ca();
    // C_new->print();

    return C_sys; // change return value to maps later
}

double Embedding::do_env(SharedWavefunction wfn, SharedMatrix hcore, Options& options) {
    // Add options for environment methods here
    // Currently HF, add functionals in the future
    int nirrep = hcore->nirrep();
    Dimension nmopi = hcore->colspi();
    Dimension noccpi = wfn->doccpi();

    // add DFT calculation for environment here
    return 0.0;
}

double Embedding::do_sys(SharedWavefunction wfn, SharedMatrix h_ainb, Options& options) {
    // Add options for system methods here
    // Currently HF, add WFs in the future
    int nirrep = wfn->nirrep();
    Dimension nmopi = wfn->nmopi();
    Dimension noccpi = wfn->doccpi();

    // add other expensive wavefunction methods here
    return 0.0;
}

std::map<std::string, SharedMatrix> Embedding::localize(SharedWavefunction wfn, Options& options) {
    // Add options for localization here
    int nirrep = wfn->nirrep();
    Dimension nmopi = wfn->nmopi();
    Dimension noccpi = wfn->doccpi();

    if (options_.get_str("LOCALIZATION_METHOD") == "IAO"|| options_.get_str("LOCALIZATION_METHOD") == "IAO_IBO") {
        // IAO localization
        outfile->Printf("\n Coeffs before localization \n");
        wfn->Ca()->print();
        std::shared_ptr<IAOBuilder> iaobd;
        iaobd = IAOBuilder::build(wfn->basisset(), wfn->get_basisset("MINAO_BASIS"), wfn->Ca(),
                                  options);
        std::map<std::string, SharedMatrix> ret = iaobd->build_iaos();
        outfile->Printf("\n ------ IAO coeffs! ------ \n");
        ret["A"]->print();
        outfile->Printf("\n ------ IAO rotation matrix U automate ------ \n");
        ret["U"]->print();
		outfile->Printf("\n ------ IAO rotation matrix U manual ------ \n");
		SharedMatrix Ctmp((new Matrix("Saved Coeffs", nirrep, nmopi, nmopi)));
		Ctmp->copy(wfn->Ca());
		Ctmp->general_invert();
		SharedMatrix U = Matrix::doublet(Ctmp, ret["A"], false, false);
		U->print();
		//U->general_invert();
		//U->print();

        SharedMatrix Cocc = build_Cocc(wfn->Ca(), nirrep, nmopi, noccpi);
        SharedMatrix Focc = build_Focc(wfn->Fa(), nirrep, nmopi, noccpi);
        std::vector<int> ranges = {};
        std::map<std::string, SharedMatrix> ret_loc = iaobd->localize(Cocc, Focc, ranges);
        outfile->Printf("iao build C occ\n");
        ret_loc["L"]->print();
        //outfile->Printf("iao build localized charge matrix \n");
        //ret_loc["Q"]->print();
        outfile->Printf("\n ------ IAO rotation matrix U (localizer) ------ \n");
        ret_loc["U"]->print();

		SharedMatrix C_loc = Matrix::doublet(wfn->Ca(), ret_loc["U"]);
		outfile->Printf("iao build C all\n");
		C_loc->print();

		outfile->Printf("\n Hello World #1 \n");
		SharedMatrix C_ret = Ctmp;
		if (options_.get_str("LOCALIZATION_METHOD") == "IAO") {
			C_ret->copy(ret["A"]);
		}
		if (options_.get_str("LOCALIZATION_METHOD") == "IAO_IBO") {
			C_ret->copy(C_loc);
		}
        std::vector<std::string> iaolabel =
            iaobd->print_IAO(C_ret, ret_loc["U"]->colspi()[0], wfn->basisset()->nbf(), wfn);

		outfile->Printf("\n Hello World #2 \n");
        int sizelab = iaolabel.size();
        std::vector<int> index_label = {};
		outfile->Printf("Hello World #3");
        for (int i = 0; i < sizelab; ++i) {
            std::string tmps = iaolabel[i];
            char tmp = tmps.at(0);
            int tmpint = tmp;
            tmpint -= 48;
            outfile->Printf("\n IAO orbital: %s, Atom number %d \n", tmps.c_str(), tmpint);
            index_label.push_back(tmpint);
        }
		outfile->Printf("Hello World #4");

        SharedMatrix Ind(new Matrix("Index Matrix", nirrep, nmopi, nmopi));
        for (int i = 0; i < nmopi[0]; ++i) {
            Ind->set(0, i, i, index_label[i]);
        }

		outfile->Printf("Hello World #5");
		ret_loc["I"] = Ind;
		ret_loc["IAO"] = C_ret;
		if (options_.get_str("LOCALIZATION_METHOD") == "IAO") {
			ret_loc["Trans"] = U;
		}
		if (options_.get_str("LOCALIZATION_METHOD") == "IAO_IBO") {
			ret_loc["Trans"] = ret_loc["U"];
		}

        return ret_loc;
    }

    // Other localization methods go here
}

double Embedding::compute_energy() {
    // Initialize calculation ~
    // Require a molecule in wfn object with subset A and B!

    std::shared_ptr<Molecule> mol = ref_wfn_->molecule();
    int nfrag = mol->nfragments();

    if (nfrag == 1) {
        outfile->Printf("Warning! A input molecule with fragments (--- in atom list) is required "
                        "for embedding!");
    }

    outfile->Printf(
        "\n The input molecule have %d fragments, assigning the first fragment as system! \n",
        nfrag);

    std::vector<int> none_list = {};
    std::vector<int> sys_list = {0};
    std::vector<int> env_list = {1}; // change to 1-nfrag in the future!

    std::shared_ptr<Molecule> mol_sys = mol->extract_subsets(sys_list, none_list);
    std::shared_ptr<Molecule> mol_env = mol->extract_subsets(env_list, none_list);
    outfile->Printf("\n System Fragment \n");
    mol_sys->print();
    outfile->Printf("\n Environment Fragment(s) \n");
    mol_env->print();

    int natom_sys = mol_sys->natom();

    // 1. Compute (environment method) energy for the whole system, currently HF
    SharedMatrix h_tot = ref_wfn_->H();
    double E_tot = do_env(ref_wfn_, h_tot, options_); // put method = 2 to skip this step now!

    // 2. Localize orbitals to IAO, Compute (environment method) energy for the whole system in IAO
    std::map<std::string, SharedMatrix> Loc = localize(ref_wfn_, options_);

    // 3. Evaluate C, decide which orbital belong to which subset, A or B, based on input molecule
    // subsets SharedMatrix C_B(new Matrix("C_local", nirrep_, nmopi_, nmopi_));
    SharedMatrix C_A = Separate_orbital_subset(ref_wfn_, natom_sys, Loc, options_);

    C_A->print();
    // 4. create wfn of system with basis and C_A
    // SharedWavefunction wfn_env(new Wavefunction(molecule_, basisset_));
    // deep_copy(ref_wfn_); //Save a copy of the original wfn!!
    // wfn_env = ref_wfn_;
    SharedMatrix C_origin((new Matrix("Saved Coeffs", nirrep_, nmopi_, nmopi_)));
    C_origin->copy(ref_wfn_->Ca());
    SharedMatrix S_origin((new Matrix("Saved Overlaps", nirrep_, nmopi_, nmopi_)));
    S_origin->copy(ref_wfn_->S());
    SharedMatrix H_origin((new Matrix("Saved Hcore", nirrep_, nmopi_, nmopi_)));
    H_origin->copy(ref_wfn_->H());
    SharedMatrix Fa_origin((new Matrix("Saved Fock", nirrep_, nmopi_, nmopi_)));
    Fa_origin->copy(ref_wfn_->Fa());

    // From here, change the ref_wfn_ to wfn of system!!!
    // molecule_->deactivate_all_fragments();
    // molecule_->set_active_fragment(0);

    outfile->Printf("\n ****** Setting up system wavefunction ****** \n");

    // rotate S
    SharedMatrix S_iao = Matrix::triplet(Loc["Trans"], S_origin, Loc["Trans"], true, false, false);
	
    // rotate h
    SharedMatrix h_iao = Matrix::triplet(Loc["Trans"], H_origin, Loc["Trans"], true, false, false);

    // rotate F
    SharedMatrix F_iao = Matrix::triplet(Loc["Trans"], Fa_origin, Loc["Trans"], true, false, false);

	//Unitary test passed
    // set molecule, failed
    //(*molecule_) = *mol_sys;
    mol_sys->print();
    //molecule_ = mol_sys;
    // ref_wfn_->molecule() = mol_sys;
    ref_wfn_->molecule()->print();
    // molecule_->set_ghost_fragment(1);
    // ref_wfn_->molecule()->print();

	//Build environment G A+B
	SharedMatrix Dab(new Matrix("D A + B", nirrep_, nmopi_, nmopi_));
	build_D(Loc["IAO"], doccpi_, Dab);
	Dab->print();
	SharedMatrix Gab = build_G_withD(ref_wfn_, Dab);
	Gab->print();

	/* Test transformation, if IAO rotation changes G_uv: passed
	SharedMatrix Dab_ref(new Matrix("D A + B", nirrep_, nmopi_, nmopi_));
	build_D(C_origin, doccpi_, Dab_ref);
	Dab_ref->print();
	SharedMatrix Gab_ref = build_G_withD(ref_wfn_, Dab);
	Gab_ref->print();
	*/

    // Test and truncate matrixes!
    Dimension nmo_sys_pi = ref_wfn_->nmopi();
    Dimension nzeropi = ref_wfn_->nmopi();
    for (int h = 0; h < nirrep_; ++h) {
        nmo_sys_pi[h] = C_A->colspi()[h];
        nzeropi[h] = nmopi_[h] - nmopi_[h];
    }
    Slice sys(nzeropi, nmo_sys_pi);
    Slice allmo(nzeropi, nmopi_);

    Dimension docc_sys_pi = doccpi_;
    docc_sys_pi[0] = options_.get_int("SYS_DOCC");

	outfile->Printf(" ------ H IAO origin ------ \n");
	h_iao->print();
	//Swap Matrices here
	
	int thresh = natom_sys + 1;
	int count_swap = 0;
	for (int i = 0; i < nmopi_[0]; ++i) {
		if (Loc["I"]->get(0, i, i) < thresh) {
			if (i == count_swap) {
				//do nothing and continue
			}
			else {
				outfile->Printf("Swap %d and %d \n", i, count_swap);
				h_iao->swap_rows(0, i, count_swap);
				h_iao->swap_columns(0, i, count_swap);
				S_iao->swap_rows(0, i, count_swap);
				S_iao->swap_columns(0, i, count_swap);
				F_iao->swap_rows(0, i, count_swap);
				F_iao->swap_columns(0, i, count_swap);
				Gab->swap_rows(0, i, count_swap);
				Gab->swap_columns(0, i, count_swap);
				//Loc["U"]->swap_rows(0, i, count_swap);
				//Loc["U"]->swap_columns(0, i, count_swap);
				++count_swap;
			}
		}
	}

	/* Debug only! test how the order of orbitals influence calculation
	outfile->Printf(" ------ Test @ swapt ! ------ \n");
	h_iao->print();
	S_iao->print();
	S_iao->swap_rows(0, 0, 3);
	S_iao->swap_columns(0, 0, 3);
	h_iao->swap_rows(0, 0, 3);
	h_iao->swap_columns(0, 0, 3);
	h_iao->print();
	S_iao->print();
	C_A->swap_columns(0, 0, 3);
	*/

	/*
	H_->copy(h_iao); //debug only! test whether iao rotation changes the HF energy: passed!
	S_->copy(S_iao);
	Ca_->copy(Loc["IAO"]);
	Fa_->copy(F_iao);
	*/

    SharedMatrix h_sys = h_iao;
    if (options_.get_str("SYSTEM_MATRIX") ==
        "SYS") { // Use small matrix, truncate every matrix to sys,sys block
		
        // Set Coeffs
        Ca_->copy(C_A->get_block(sys, sys));

        // Set overlaps
        S_->copy(S_iao->get_block(sys, sys));

        // Set hcore
        h_sys->copy(h_iao->get_block(sys, sys));
        H_->copy(h_sys); // wfn now have a iao based h_sys without embedding

        // Set Fock
        Fa_->copy(F_iao->get_block(sys, sys));

        // Set dimensions and MO_space_info
        // nmopi_ = nmo_sys_pi;

        // Set all const
        // nmo_ = nmo_sys_pi[0];
        // nalpha_ = docc_sys_pi[0];
        // nbeta_ = docc_sys_pi[0];
        // outfile->Printf("\n !!! System num of MOs: %d, occ MOs: %d, alpha electron: %d, beta
        // electron: %d \n", 	ref_wfn_->nmopi()[0], ref_wfn_->doccpi()[0], ref_wfn_->nalpha(),
        // ref_wfn_->nbeta());

        // Now ref_wfn_ has been wfn of system (with full basis)!
        outfile->Printf("\n ****** System wavefunction Set! ****** \n");
        ref_wfn_->molecule()->print();
        ref_wfn_->S()->print();
        ref_wfn_->Ca()->print();
        ref_wfn_->H()->print();
        ref_wfn_->Fa()->print();
        outfile->Printf("Number of system occupied orbital: %d", ref_wfn_->doccpi()[0]);
        outfile->Printf("Size of calculation matrix: %d", ref_wfn_->nmopi()[0]);

    }

    if (options_.get_str("SYSTEM_MATRIX") ==
        "ALL") { // Use Large matrix, put other parts zero, use 7*14 (14*14 with zero) Coeff matrix
        // Set Coeffs
        Ca_->zero();

        if (options_.get_str("C_SIZE") == "SYS") {
            SharedMatrix Ctmp = C_A->get_block(sys, sys);
            Ca_->set_block(sys, sys, Ctmp);
        }

        if (options_.get_str("C_SIZE") == "ALL") {
            Ca_->set_block(sys, allmo, C_A);
        }

        // Set overlaps
        // outfile->Printf("\n ------ S Original ------ \n");
        // S_->print();
        // outfile->Printf("\n ------ S IAO ------ \n");
        // S_iao->print();
        SharedMatrix Stmp = S_iao->get_block(sys, sys);
        S_->zero();
        S_->set_block(sys, sys, Stmp);

        // put 1 in env,env block diagonal to ensure decomposition quality
        for (int i = C_A->colspi()[0]; i < nmopi_[0]; ++i) {
            S_->set(0, i, i, 1.0);
        }
        outfile->Printf("\n ------ S iao truncated to system block (large matrix) ------ \n");
        S_->print();

        // Set hcore
        SharedMatrix htmp = h_iao->get_block(sys, sys);
        h_sys->zero();
        h_sys->set_block(sys, sys, htmp);
        H_->copy(h_sys);
        // wfn now have a iao based h_sys without embedding

        // Set Fock
        outfile->Printf("\n ------ F rotated to iao, and truncate to system block ------ \n");
        SharedMatrix Ftmp = F_iao->get_block(sys, sys);
        Fa_->zero();
        Fa_->set_block(sys, sys, Ftmp);
        // Fa_->print();

        // Set dimensions
        ref_wfn_->force_doccpi(docc_sys_pi);
        // doccpi_[0] = 5;
        // ref_wfn_->doccpi()[0] = 5; //Only for water-dimer water_sys!!!

        // Set MO_space_info

        // Now ref_wfn_ has been wfn of system (with full basis)!
        outfile->Printf("\n ****** System wavefunction Set! ****** \n");
        ref_wfn_->molecule()->print();
        ref_wfn_->S()->print();
        ref_wfn_->Ca()->print();
        ref_wfn_->H()->print();
        ref_wfn_->Fa()->print();
        outfile->Printf("\n Number of system occupied orbital: %d", ref_wfn_->doccpi()[0]);
        outfile->Printf("\n Size of calculation matrix: %d", ref_wfn_->nmopi()[0]);
    }

    // 5. Evaluate G(A + B) and G(A), evaluate and project h A-in-B (function)

    // ref_wfn_->force_doccpi(docc_sys_pi); //Only for water-dimer water_sys!!!
	
    SharedMatrix Da(new Matrix("D A", nirrep_, nmo_sys_pi, nmo_sys_pi)); //Change back to nmo_sys_pi after tests!
    build_D(Ca_, docc_sys_pi, Da);
    Da->print();
    SharedMatrix Ga = build_G_withD(ref_wfn_, Da);

    // Set Density
    Da_->copy(Da);

    SharedMatrix Gab_sys = Gab->get_block(sys, sys);
    outfile->Printf("\n ------ Building H A-in-B ------ \n");
	//outfile->Printf(" ------ H IAO ------ \n");
	//h_iao->print();
	outfile->Printf(" ------ H IAO sys ------ \n");
	h_sys->print();
	outfile->Printf(" ------ G A+B ------ \n");
	Gab_sys->print();
    h_sys->add(Gab_sys);
	outfile->Printf(" ------ G A ------ \n");
	Ga->print();
    h_sys->subtract(Ga); // build h A-in-B !!
	outfile->Printf(" ------ H IAO sys emb A-in-B ------ \n");
    h_sys->print();

    // 6. Compute (cheap environment method) energy for system A, with h A-in-B
    // Known issue: JKbuilder and Mintshelper will die if the size of basis and size of matrix
    // discoherent!
    //outfile->Printf("\n ------ calculating E_sys_cheap ------ \n");
    double E_sys_cheap = do_env(ref_wfn_, h_sys, options_);
    //double E_sys_ref = do_env(ref_wfn_, H_, options_);
    H_->copy(h_sys); // set H in wfn to be embedded h_sys

    // 7. Compute （expensive system method) energy for system A, with h A-in-B
    //outfile->Printf("\n ------ calculating E_sys_exp ------ \n");
    //double E_sys_exp = do_sys(ref_wfn_, h_sys, options_);

    // Return (now environment) Energy
    // double E_embedding = E_tot - E_sys_cheap + E_sys_exp;
    //outfile->Printf(
    //    "\n ------ (insert method string here) Cheap method system energy: E = %8.8f ------ \n",
    //    E_sys_cheap);
    return E_sys_cheap;
}

Embedding::~Embedding() {}
} // namespace forte
} // namespace psi
