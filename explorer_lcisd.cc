#include "explorer.h"

#include <cmath>
#include <functional>
#include <algorithm>

#include <boost/timer.hpp>
#include <boost/format.hpp>

#include <libqt/qt.h>


#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <libciomr/libciomr.h>
//#include <libqt/qt.h>

#include "explorer.h"
#include "cartographer.h"
#include "string_determinant.h"

using namespace std;
using namespace psi;

namespace psi{ namespace libadaptive{

/**
 * Diagonalize the
 */
void Explorer::lambda_mrcisd(psi::Options& options)
{
    fprintf(outfile,"\n\n  Lambda-MRCISD");

    int nroot = options.get_int("NROOT");

    double selection_threshold = t2_threshold_;

    fprintf(outfile,"\n\n  Diagonalizing the Hamiltonian in the model space (Lambda = %.2f Eh)\n",space_m_threshold_);

    bool aimed_selection = false;
    bool energy_select = false;
    if (options.get_str("SELECT_TYPE") == "AIMED_AMP"){
        aimed_selection = true;
        energy_select = false;
    }else if (options.get_str("SELECT_TYPE") == "AIMED_ENERGY"){
        aimed_selection = true;
        energy_select = true;
    }else if(options.get_str("SELECT_TYPE") == "ENERGY"){
        aimed_selection = false;
        energy_select = true;
    }else if(options.get_str("SELECT_TYPE") == "AMP"){
        aimed_selection = false;
        energy_select = false;
    }

    SharedMatrix H;
    SharedMatrix evecs;
    SharedVector evals;

    // 1) Build the Hamiltonian using the StringDeterminant representation
    std::vector<StringDeterminant> ref_space;
    std::map<StringDeterminant,int> ref_space_map;

    for (size_t I = 0, maxI = determinants_.size(); I < maxI; ++I){
        boost::tuple<double,int,int,int,int>& determinantI = determinants_[I];
        const int I_class_a = determinantI.get<1>();  //std::get<1>(determinantI);
        const int Isa = determinantI.get<2>();        //std::get<1>(determinantI);
        const int I_class_b = determinantI.get<3>(); //std::get<2>(determinantI);
        const int Isb = determinantI.get<4>();        //std::get<2>(determinantI);
        StringDeterminant det(vec_astr_symm_[I_class_a][Isa].get<2>(),vec_bstr_symm_[I_class_b][Isb].get<2>());
        ref_space.push_back(det);
        ref_space_map[det] = 1;
    }

    size_t dim_ref_space = ref_space.size();

    fprintf(outfile,"\n  The model space contains %zu determinants",dim_ref_space);
    fflush(outfile);

    H.reset(new Matrix("Hamiltonian Matrix",dim_ref_space,dim_ref_space));
    evecs.reset(new Matrix("U",dim_ref_space,nroot));
    evals.reset(new Vector("e",nroot));

    boost::timer t_h_build;
#pragma omp parallel for schedule(dynamic)
    for (size_t I = 0; I < dim_ref_space; ++I){
        const StringDeterminant& detI = ref_space[I];
        for (size_t J = I; J < dim_ref_space; ++J){
            const StringDeterminant& detJ = ref_space[J];
            double HIJ = detI.slater_rules(detJ);
            H->set(I,J,HIJ);
            H->set(J,I,HIJ);
        }
    }
    fprintf(outfile,"\n  Time spent building H               = %f s",t_h_build.elapsed());
    fflush(outfile);

    // 4) Diagonalize the Hamiltonian
    boost::timer t_hdiag_large;
    if (options.get_str("DIAG_ALGORITHM") == "DAVIDSON"){
        fprintf(outfile,"\n  Using the Davidson-Liu algorithm.");
        davidson_liu(H,evals,evecs,nroot);
    }else if (options.get_str("DIAG_ALGORITHM") == "FULL"){
        fprintf(outfile,"\n  Performing full diagonalization.");
        H->diagonalize(evecs,evals);
    }

    fprintf(outfile,"\n  Time spent diagonalizing H          = %f s",t_hdiag_large.elapsed());
    fflush(outfile);

    // 5) Print the energy
    for (int i = 0; i < nroot; ++ i){
        fprintf(outfile,"\n  Ren. step CI Energy Root %3d = %.12f Eh = %8.4f eV",i + 1,evals->get(i) + nuclear_repulsion_energy_,27.211 * (evals->get(i) - evals->get(0)));
        //        fprintf(outfile,"\n  Ren. step CI Energy + EPT2 Root %3d = %.12f = %.12f + %.12f",i + 1,evals->get(i) + multistate_pt2_energy_correction_[i],
        //                evals->get(i),multistate_pt2_energy_correction_[i]);
    }
    fflush(outfile);


    int nmo = reference_determinant_.nmo();
    size_t nfrzc = frzc_.size();
    size_t nfrzv = frzv_.size();

    std::vector<int> aocc(nalpha_ - nfrzc);
    std::vector<int> bocc(nbeta_ - nfrzc);
    std::vector<int> avir(nmo_ - nalpha_ - nfrzv);
    std::vector<int> bvir(nmo_ - nbeta_ - nfrzv);

    int noalpha = nalpha_ - nfrzc;
    int nobeta  = nbeta_ - nfrzc;
    int nvalpha = nmo_ - nalpha_;
    int nvbeta  = nmo_ - nbeta_;

    // Find the SD space out of the reference
    std::vector<StringDeterminant> sd_dets_vec;
    std::map<StringDeterminant,int> new_dets_map;
    boost::timer t_ms_build;

    for (size_t I = 0, max_I = ref_space_map.size(); I < max_I; ++I){
        const StringDeterminant& det = ref_space[I];
        for (int p = 0, i = 0, a = 0; p < nmo_; ++p){
            if (det.get_alfa_bit(p)){
                if (std::count (frzc_.begin(),frzc_.end(),p) == 0){
                    aocc[i] = p;
                    i++;
                }
            }else{
                if (std::count (frzv_.begin(),frzv_.end(),p) == 0){
                    avir[a] = p;
                    a++;
                }
            }
        }
        for (int p = 0, i = 0, a = 0; p < nmo_; ++p){
            if (det.get_beta_bit(p)){
                if (std::count (frzc_.begin(),frzc_.end(),p) == 0){
                    bocc[i] = p;
                    i++;
                }
            }else{
                if (std::count (frzv_.begin(),frzv_.end(),p) == 0){
                    bvir[a] = p;
                    a++;
                }
            }
        }

        // Generate aa excitations
        for (int i = 0; i < noalpha; ++i){
            int ii = aocc[i];
            for (int a = 0; a < nvalpha; ++a){
                int aa = avir[a];
                if ((mo_symmetry_[ii] ^ mo_symmetry_[aa]) == wavefunction_symmetry_){
                    StringDeterminant new_det(det);
                    new_det.set_alfa_bit(ii,false);
                    new_det.set_alfa_bit(aa,true);
                    if(ref_space_map.find(new_det) == ref_space_map.end()){
                        sd_dets_vec.push_back(new_det);
                    }
                }
            }
        }

        for (int i = 0; i < nobeta; ++i){
            int ii = bocc[i];
            for (int a = 0; a < nvbeta; ++a){
                int aa = bvir[a];
                if ((mo_symmetry_[ii] ^ mo_symmetry_[aa])  == wavefunction_symmetry_){
                    StringDeterminant new_det(det);
                    new_det.set_beta_bit(ii,false);
                    new_det.set_beta_bit(aa,true);
                    if(ref_space_map.find(new_det) == ref_space_map.end()){
                        sd_dets_vec.push_back(new_det);
                    }
                }
            }
        }

        // Generate aa excitations
        for (int i = 0; i < noalpha; ++i){
            int ii = aocc[i];
            for (int j = i + 1; j < noalpha; ++j){
                int jj = aocc[j];
                for (int a = 0; a < nvalpha; ++a){
                    int aa = avir[a];
                    for (int b = a + 1; b < nvalpha; ++b){
                        int bb = avir[b];
                        if ((mo_symmetry_[ii] ^ mo_symmetry_[jj] ^ mo_symmetry_[aa] ^ mo_symmetry_[bb]) == wavefunction_symmetry_){
                            StringDeterminant new_det(det);
                            new_det.set_alfa_bit(ii,false);
                            new_det.set_alfa_bit(jj,false);
                            new_det.set_alfa_bit(aa,true);
                            new_det.set_alfa_bit(bb,true);
                            if(ref_space_map.find(new_det) == ref_space_map.end()){
                                sd_dets_vec.push_back(new_det);
                            }
                        }
                    }
                }
            }
        }

        for (int i = 0; i < noalpha; ++i){
            int ii = aocc[i];
            for (int j = 0; j < nobeta; ++j){
                int jj = bocc[j];
                for (int a = 0; a < nvalpha; ++a){
                    int aa = avir[a];
                    for (int b = 0; b < nvbeta; ++b){
                        int bb = bvir[b];
                        if ((mo_symmetry_[ii] ^ mo_symmetry_[jj] ^ mo_symmetry_[aa] ^ mo_symmetry_[bb]) == wavefunction_symmetry_){
                            StringDeterminant new_det(det);
                            new_det.set_alfa_bit(ii,false);
                            new_det.set_beta_bit(jj,false);
                            new_det.set_alfa_bit(aa,true);
                            new_det.set_beta_bit(bb,true);
                            if(ref_space_map.find(new_det) == ref_space_map.end()){
                                sd_dets_vec.push_back(new_det);
                            }
                        }
                    }
                }
            }
        }
        for (int i = 0; i < nobeta; ++i){
            int ii = bocc[i];
            for (int j = i + 1; j < nobeta; ++j){
                int jj = bocc[j];
                for (int a = 0; a < nvbeta; ++a){
                    int aa = bvir[a];
                    for (int b = a + 1; b < nvbeta; ++b){
                        int bb = bvir[b];
                        if ((mo_symmetry_[ii] ^ mo_symmetry_[jj] ^ mo_symmetry_[aa] ^ mo_symmetry_[bb]) == wavefunction_symmetry_){
                            StringDeterminant new_det(det);
                            new_det.set_beta_bit(ii,false);
                            new_det.set_beta_bit(jj,false);
                            new_det.set_beta_bit(aa,true);
                            new_det.set_beta_bit(bb,true);
                            if(ref_space_map.find(new_det) == ref_space_map.end()){
                                sd_dets_vec.push_back(new_det);
                            }
                        }
                    }
                }
            }
        }
    }

    fprintf(outfile,"\n  The SD excitation space has dimension: %zu",sd_dets_vec.size());

    boost::timer t_ms_screen;

    sort( sd_dets_vec.begin(), sd_dets_vec.end() );
    sd_dets_vec.erase( unique( sd_dets_vec.begin(), sd_dets_vec.end() ), sd_dets_vec.end() );

    fprintf(outfile,"\n  The SD excitation space has dimension: %zu (unique)",sd_dets_vec.size());
    fprintf(outfile,"\n  Time spent building the model space = %f s",t_ms_build.elapsed());
    fflush(outfile);

    // This will contain all the determinants
    std::vector<StringDeterminant> ref_sd_dets;
    for (size_t J = 0, max_J = ref_space.size(); J < max_J; ++J){
        ref_sd_dets.push_back(ref_space[J]);
    }


    // Check the coupling between the reference and the SD space
    std::vector<std::pair<double,size_t> > new_dets_importance_vec;

    std::vector<double> V(nroot,0.0);
    std::vector<std::pair<double,double> > C1(nroot,make_pair(0.0,0.0));
    std::vector<std::pair<double,double> > E2(nroot,make_pair(0.0,0.0));
    std::vector<double> ept2(nroot,0.0);

    double aimed_selection_sum = 0.0;

    for (size_t I = 0, max_I = sd_dets_vec.size(); I < max_I; ++I){
        double EI = sd_dets_vec[I].energy();
        for (int n = 0; n < nroot; ++n){
            V[n] = 0;
        }
        for (size_t J = 0, max_J = ref_space.size(); J < max_J; ++J){
            double HIJ = sd_dets_vec[I].slater_rules(ref_space[J]);
            for (int n = 0; n < nroot; ++n){
                V[n] += evecs->get(J,n) * HIJ;
            }
        }
        for (int n = 0; n < nroot; ++n){
            double C1_I = -V[n] / (EI - evals->get(n));
            double E2_I = -V[n] * V[n] / (EI - evals->get(n));
            C1[n] = make_pair(std::fabs(C1_I),C1_I);
            E2[n] = make_pair(std::fabs(E2_I),E2_I);
        }

        //        double C1 = std::fabs(V / (EI - evals->get(0)));
        //        double E2 = std::fabs(V * V / (EI - evals->get(0)));

        //        double select_value = (energy_select ? E2 : C1);

        std::pair<double,double> max_C1 = *std::max_element(C1.begin(),C1.end());
        std::pair<double,double> max_E2 = *std::max_element(E2.begin(),E2.end());

        double select_value = energy_select ? max_E2.first : max_C1.first;

        // Do not select now, just store the determinant index and the selection criterion
        if(aimed_selection){
            if (energy_select){
                new_dets_importance_vec.push_back(std::make_pair(select_value,I));
                aimed_selection_sum += select_value;
            }else{
                new_dets_importance_vec.push_back(std::make_pair(select_value * select_value,I));
                aimed_selection_sum += select_value * select_value;
            }
        }else{
            if (std::fabs(select_value) > t2_threshold_){
                new_dets_importance_vec.push_back(std::make_pair(select_value,I));
            }else{
                for (int n = 0; n < nroot; ++n) ept2[n] += E2[n].second;
            }
        }
    }

    if(aimed_selection){
        std::sort(new_dets_importance_vec.begin(),new_dets_importance_vec.end());
        std::reverse(new_dets_importance_vec.begin(),new_dets_importance_vec.end());
        size_t maxI = new_dets_importance_vec.size();
        fprintf(outfile,"\n  The SD space will be generated using the aimed scheme (%s)",energy_select ? "energy" : "amplitude");
        fprintf(outfile,"\n  Initial value of sigma in the aimed selection = %24.14f",aimed_selection_sum);
        for (size_t I = 0; I < maxI; ++I){
            if (aimed_selection_sum > t2_threshold_){
                ref_sd_dets.push_back(sd_dets_vec[new_dets_importance_vec[I].second]);
                aimed_selection_sum -= new_dets_importance_vec[I].first;
            }else{
                break;
            }
        }
        fprintf(outfile,"\n  Final value of sigma in the aimed selection   = %24.14f",aimed_selection_sum);
        fprintf(outfile,"\n  Selected %zu determinants",ref_sd_dets.size()-ref_space.size());
    }else{
        fprintf(outfile,"\n  The SD space will be generated by screening (%s)",energy_select ? "energy" : "amplitude");
        size_t maxI = new_dets_importance_vec.size();
        for (size_t I = 0; I < maxI; ++I){
            ref_sd_dets.push_back(sd_dets_vec[new_dets_importance_vec[I].second]);
        }
    }

    multistate_pt2_energy_correction_ = ept2;

    size_t dim_ref_sd_dets = ref_sd_dets.size();

    fprintf(outfile,"\n  After screening the Lambda-CISD space contains %zu determinants",dim_ref_sd_dets);
    fprintf(outfile,"\n  Time spent screening the model space = %f s",t_ms_screen.elapsed());
    fflush(outfile);


    evecs.reset(new Matrix("U",dim_ref_sd_dets,nroot));
    evals.reset(new Vector("e",nroot));
    // Full algorithm
    if (options.get_str("ENERGY_TYPE") == "LMRCISD"){
        H.reset(new Matrix("Hamiltonian Matrix",dim_ref_sd_dets,dim_ref_sd_dets));

        boost::timer t_h_build2;
#pragma omp parallel for schedule(dynamic)
        for (size_t I = 0; I < dim_ref_sd_dets; ++I){
            const StringDeterminant& detI = ref_sd_dets[I];
            for (size_t J = I; J < dim_ref_sd_dets; ++J){
                const StringDeterminant& detJ = ref_sd_dets[J];
                double HIJ = detI.slater_rules(detJ);
                if (I == J) HIJ += nuclear_repulsion_energy_;
                H->set(I,J,HIJ);
                H->set(J,I,HIJ);
            }
        }
        fprintf(outfile,"\n  Time spent building H               = %f s",t_h_build2.elapsed());
        fflush(outfile);

        // 4) Diagonalize the Hamiltonian
        boost::timer t_hdiag_large2;
        if (options.get_str("DIAG_ALGORITHM") == "DAVIDSON"){
            fprintf(outfile,"\n  Using the Davidson-Liu algorithm.");
            davidson_liu(H,evals,evecs,nroot);
        }else if (options.get_str("DIAG_ALGORITHM") == "FULL"){
            fprintf(outfile,"\n  Performing full diagonalization.");
            H->diagonalize(evecs,evals);
        }

        fprintf(outfile,"\n  Time spent diagonalizing H          = %f s",t_hdiag_large2.elapsed());
        fflush(outfile);
    }
    // Sparse algorithm
    else{
        boost::timer t_h_build2;
        std::vector<std::vector<std::pair<int,double> > > H_sparse;

        size_t num_nonzero = 0;
        // Form the Hamiltonian matrix
        for (size_t I = 0; I < dim_ref_sd_dets; ++I){
            std::vector<std::pair<int,double> > H_row;
            const StringDeterminant& detI = ref_sd_dets[I];
            double HII = detI.slater_rules(detI) + nuclear_repulsion_energy_;
            H_row.push_back(make_pair(int(I),HII));
            for (size_t J = 0; J < dim_ref_sd_dets; ++J){
                if (I != J){
                    const StringDeterminant& detJ = ref_sd_dets[J];
                    double HIJ = detI.slater_rules(detJ);
                    if (std::fabs(HIJ) >= 1.0e-12){
                        H_row.push_back(make_pair(int(J),HIJ));
                        num_nonzero += 1;
                    }
                }
            }
            H_sparse.push_back(H_row);
        }

        fprintf(outfile,"\n  %ld nonzero elements out of %ld (%e)",num_nonzero,size_t(dim_ref_sd_dets * dim_ref_sd_dets),double(num_nonzero)/double(dim_ref_sd_dets * dim_ref_sd_dets));
        fprintf(outfile,"\n  Time spent building H               = %f s",t_h_build2.elapsed());
        fflush(outfile);

        // 4) Diagonalize the Hamiltonian
        boost::timer t_hdiag_large2;
        fprintf(outfile,"\n  Using the Davidson-Liu algorithm.");
        davidson_liu_sparse(H_sparse,evals,evecs,nroot);
        fprintf(outfile,"\n  Time spent diagonalizing H          = %f s",t_hdiag_large2.elapsed());
        fflush(outfile);
    }
    fprintf(outfile,"\n  Finished building H");

    fprintf(outfile,"\n\n  => Lambda+SD-CI <=\n");
    // 5) Print the energy
    for (int i = 0; i < nroot; ++ i){
        fprintf(outfile,"\n  Adaptive CI Energy Root %3d        = %20.12f Eh = %8.4f eV",i + 1,evals->get(i),27.211 * (evals->get(i) - evals->get(0)));
        fprintf(outfile,"\n  Adaptive CI Energy + EPT2 Root %3d = %20.12f Eh = %8.4f eV",i + 1,evals->get(i) + multistate_pt2_energy_correction_[i],
                27.211 * (evals->get(i) - evals->get(0) + multistate_pt2_energy_correction_[i] - multistate_pt2_energy_correction_[0]));
    }
    fflush(outfile);

    // Set some environment variables
    Process::environment.globals["LAMBDA+SD-CI ENERGY"] = evals->get(options_.get_int("ROOT"));

    print_results_lambda_sd_ci(ref_sd_dets,evecs,evals,nroot);
    fflush(outfile);
}

void Explorer::print_results_lambda_sd_ci(vector<StringDeterminant>& determinants,
                                          SharedMatrix evecs,
                                          SharedVector evals,
                                          int nroots)
{
    std::vector<string> s2_labels({"singlet","doublet","triplet","quartet","quintet","sextet","septet","octet","nonet"});

    int nroots_print = std::min(nroots,25);

    fprintf(outfile,"\n\n  => Summary of results <=\n");


    for (int i = 0; i < nroots_print; ++ i){
        // Find the most significant contributions to this root
        size_t ndets = evecs->nrow();
        std::vector<std::pair<double,int> > C_J_sorted;

        double significant_threshold = 0.00001;
        double significant_wave_function = 0.99999;

        double** C_mat = evecs->pointer();
        for (int J = 0; J < ndets; ++J){
            if (std::fabs(C_mat[J][i]) > significant_threshold){
                C_J_sorted.push_back(make_pair(std::fabs(C_mat[J][i]),J));
            }
        }

        // Sort them and
        int num_sig = 0;
        std::sort(C_J_sorted.begin(),C_J_sorted.end(),std::greater<std::pair<double,int> >());
        double cum_wfn = 0.0;
        for (size_t I = 0, max_I = C_J_sorted.size(); I < max_I; ++I){
            int J = C_J_sorted[I].second;
            cum_wfn += C_mat[J][i] * C_mat[J][i];
            num_sig++;
            if (cum_wfn > significant_wave_function) break;
        }
        fprintf(outfile,"\nAnalysis on %d out of %zu sorted (%zu total)",num_sig,C_J_sorted.size(),determinants.size());

        double norm = 0.0;
        double S2 = 0.0;
        for (int sI = 0; sI < num_sig; ++sI){
            int I = C_J_sorted[sI].second;
            for (int sJ = 0; sJ < num_sig; ++sJ){
                int J = C_J_sorted[sJ].second;
                if (std::fabs(C_mat[I][i] * C_mat[J][i]) > 1.0e-12){
                    const double S2IJ = determinants[I].spin2(determinants[J]);
                    S2 += C_mat[I][i] * S2IJ * C_mat[J][i];
                }
            }
            norm += C_mat[I][i] * C_mat[I][i];
        }
        S2 /= norm;
        double S = std::fabs(0.5 * (std::sqrt(1.0 + 4.0 * S2) - 1.0));
        std::string state_label = s2_labels[std::round(S * 2.0)];
        fprintf(outfile,"\n  Adaptive CI Energy Root %3d = %20.12f Eh = %8.4f eV (S^2 = %5.3f, S = %5.3f, %s)",i + 1,evals->get(i),27.211 * (evals->get(i) - evals->get(0)),S2,S,state_label.c_str());
        fflush(outfile);
    }

    // 6) Print the major contributions to the eigenvector
    double significant_threshold = 0.001;
    double significant_wave_function = 0.95;
    for (int i = 0; i < nroots_print; ++ i){
        fprintf(outfile,"\n\n  => Root %3d <=\n\n  Determinants contribution to %.0f%% of the wave function:",i+1,100.0 * significant_wave_function);
        // Identify all contributions with |C_J| > significant_threshold
        double** C_mat = evecs->pointer();
        std::vector<std::pair<double,int> > C_J_sorted;
        size_t ndets = evecs->nrow();
        for (int J = 0; J < ndets; ++J){
            if (std::fabs(C_mat[J][i]) > significant_threshold){
                C_J_sorted.push_back(make_pair(std::fabs(C_mat[J][i]),J));
            }
        }
        // Sort them and print
        std::sort(C_J_sorted.begin(),C_J_sorted.end(),std::greater<std::pair<double,int> >());
        double cum_wfn = 0.0;
        int num_sig = 0;
        for (size_t I = 0, max_I = C_J_sorted.size(); I < max_I; ++I){
            int J = C_J_sorted[I].second;
            fprintf(outfile,"\n %3ld   %+9.6f   %9.6f   %d",I,C_mat[J][i],C_mat[J][i] * C_mat[J][i],J);
            cum_wfn += C_mat[J][i] * C_mat[J][i];
            num_sig++;
            if (cum_wfn > significant_wave_function) break;
        }

        // Compute the density matrices
        std::fill(Da_.begin(), Da_.end(), 0.0);
        std::fill(Db_.begin(), Db_.end(), 0.0);
        double norm = 0.0;
        for (int I = 0; I < ndets; ++I){
            double w = C_mat[I][i] * C_mat[I][i];
            if (w > 1.0e-12){
                determinants[I].diag_opdm(Da_,Db_,w);
            }
            norm += C_mat[I][i] * C_mat[I][i];
        }
//        fprintf(outfile,"\n  2-norm of the CI vector: %f",norm);
        for (int p = 0; p < nmo_; ++p){
            Da_[p] /= norm;
            Db_[p] /= norm;
        }
        fprintf(outfile,"\n\n  Occupation numbers");
        double na = 0.0;
        double nb = 0.0;
        for (int h = 0, p = 0; h < nirrep_; ++h){
            for (int n = 0; n < nmopi_[h]; ++n){
                fprintf(outfile,"\n  %4d  %1d  %4d   %5.3f    %5.3f",p+1,h,n,Da_[p],Db_[p]);
                na += Da_[p];
                nb += Db_[p];
                p += 1;
            }
        }
        fprintf(outfile,"\n  Total number of alpha/beta electrons: %f/%f",na,nb);

        fflush(outfile);
    }
}

}} // EndNamespaces

