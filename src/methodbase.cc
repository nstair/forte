#include "methodbase.h"

#include <libpsio/psio.h>
#include <libpsio/psio.hpp>

namespace psi{ namespace libadaptive{

using namespace ambit;

MethodBase::MethodBase(boost::shared_ptr<Wavefunction> wfn, Options &options, ExplorerIntegrals* ints)
    : Wavefunction(options,_default_psio_lib_), ints_(ints), tensor_type_(kCore)
{
    // Copy the wavefunction information
    copy(wfn);
//    Tensor::set_print_level(debug_);
    startup();
}

MethodBase::~MethodBase()
{
    cleanup();
}

void MethodBase::startup()
{
    double frozen_core_energy = ints_->frozen_core_energy();
    E0_ = reference_energy();

    BlockedTensor::set_expert_mode(true);

    Dimension ncmopi_ = ints_->ncmopi();

    Dimension corr_docc(doccpi_);
    corr_docc -= frzcpi_;

    for (int h = 0, p = 0; h < nirrep_; ++h){
        for (int i = 0; i < corr_docc[h]; ++i,++p){
            a_occ_mos.push_back(p);
            b_occ_mos.push_back(p);
        }
        for (int i = 0; i < soccpi_[h]; ++i,++p){
            a_occ_mos.push_back(p);
            b_vir_mos.push_back(p);
        }
        for (int a = 0; a < ncmopi_[h] - corr_docc[h] - soccpi_[h]; ++a,++p){
            a_vir_mos.push_back(p);
            b_vir_mos.push_back(p);
        }
    }

    for (size_t p = 0; p < a_occ_mos.size(); ++p) mos_to_aocc[a_occ_mos[p]] = p;
    for (size_t p = 0; p < b_occ_mos.size(); ++p) mos_to_bocc[b_occ_mos[p]] = p;
    for (size_t p = 0; p < a_vir_mos.size(); ++p) mos_to_avir[a_vir_mos[p]] = p;
    for (size_t p = 0; p < b_vir_mos.size(); ++p) mos_to_bvir[b_vir_mos[p]] = p;

    size_t naocc = a_occ_mos.size();
    size_t nbocc = b_occ_mos.size();
    size_t navir = a_vir_mos.size();
    size_t nbvir = b_vir_mos.size();

    BlockedTensor::add_mo_space("o","ijklmn",a_occ_mos,AlphaSpin);
    BlockedTensor::add_mo_space("O","IJKLMN",b_occ_mos,BetaSpin);
    BlockedTensor::add_mo_space("v","abcdef",a_vir_mos,AlphaSpin);
    BlockedTensor::add_mo_space("V","ABCDEF",b_vir_mos,BetaSpin);
    BlockedTensor::add_composite_mo_space("i","pqrstuvwxyz",{"o","v"});
    BlockedTensor::add_composite_mo_space("I","PQRSTUVWXYZ",{"O","V"});


    H = BlockedTensor::build(tensor_type_,"H",spin_cases({"ii"}));
    G1 = BlockedTensor::build(tensor_type_,"G1",spin_cases({"oo"}));
    CG1 = BlockedTensor::build(tensor_type_,"CG1",spin_cases({"vv"}));
    F = BlockedTensor::build(tensor_type_,"F",spin_cases({"ii"}));
    V = BlockedTensor::build(tensor_type_,"V",spin_cases({"iiii"}));
    InvD1 = BlockedTensor::build(tensor_type_,"Inverse D1",spin_cases({"ov"}));
    InvD2 = BlockedTensor::build(tensor_type_,"Inverse D2",spin_cases({"oovv"}));

    // Fill in the one-electron operator (H)
    H.iterate([&](const std::vector<size_t>& i,const std::vector<SpinType>& spin,double& value){
        if (spin[0] == AlphaSpin)
            value = ints_->oei_a(i[0],i[1]);
        else
            value = ints_->oei_b(i[0],i[1]);
    });

    // Fill in the two-electron operator (V)
    V.iterate([&](const std::vector<size_t>& i,const std::vector<SpinType>& spin,double& value){
        if ((spin[0] == AlphaSpin) and (spin[1] == AlphaSpin)) value = ints_->aptei_aa(i[0],i[1],i[2],i[3]);
        if ((spin[0] == AlphaSpin) and (spin[1] == BetaSpin) ) value = ints_->aptei_ab(i[0],i[1],i[2],i[3]);
        if ((spin[0] == BetaSpin)  and (spin[1] == BetaSpin) ) value = ints_->aptei_bb(i[0],i[1],i[2],i[3]);
    });

    G1.iterate([&](const std::vector<size_t>& i,const std::vector<SpinType>& spin,double& value){
        value = i[0] == i[1] ? 1.0 : 0.0;});

    CG1.iterate([&](const std::vector<size_t>& i,const std::vector<SpinType>& spin,double& value){
        value = i[0] == i[1] ? 1.0 : 0.0;});

    // Form the Fock matrix
    F["pq"]  = H["pq"];
    F["pq"] += V["prqs"] * G1["sr"];
    F["pq"] += V["pRqS"] * G1["SR"];

    F["PQ"] += H["PQ"];
    F["PQ"] += V["rPsQ"] * G1["sr"];
    F["PQ"] += V["PRQS"] * G1["SR"];

//    if (print_ > 2){
//        G1.print();
//        CG1.print();
//        H.print();
//        F.print();
//    }

    size_t ncmo_ = ints_->ncmo();
    std::vector<double> Fa(ncmo_);
    std::vector<double> Fb(ncmo_);

    F.iterate([&](const std::vector<size_t>& i,const std::vector<SpinType>& spin,double& value){
        if (spin[0] == AlphaSpin and (i[0] == i[1])){
            Fa[i[0]] = value;
        }
        if (spin[0] == BetaSpin and (i[0] == i[1])){
            Fb[i[0]] = value;
        }
    });

    InvD1.iterate([&](const std::vector<size_t>& i,const std::vector<SpinType>& spin,double& value){
        if (spin[0] == AlphaSpin){
            value = 1.0 / (Fa[i[0]] - Fa[i[1]]);
        }else if (spin[0]  == BetaSpin){
            value = 1.0 / (Fb[i[0]] - Fb[i[1]]);
        }
    });

    InvD2.iterate([&](const std::vector<size_t>& i,const std::vector<SpinType>& spin,double& value){
        if ((spin[0] == AlphaSpin) and (spin[1] == AlphaSpin)){
            value = 1.0 / (Fa[i[0]] + Fa[i[1]] - Fa[i[2]] - Fa[i[3]]);
        }else if ((spin[0] == AlphaSpin) and (spin[1] == BetaSpin) ){
            value = 1.0 / (Fa[i[0]] + Fb[i[1]] - Fa[i[2]] - Fb[i[3]]);
        }else if ((spin[0] == BetaSpin)  and (spin[1] == BetaSpin) ){
            value = 1.0 / (Fb[i[0]] + Fb[i[1]] - Fb[i[2]] - Fb[i[3]]);
        }
    });
}

void MethodBase::cleanup()
{
    BlockedTensor::set_expert_mode(false);
}

}} // End Namespaces