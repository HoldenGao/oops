#include "include/app/cce.h"

////////////////////////////////////////////////////////////////////////////////
//{{{  CCE
CCE::CCE(int my_rank, int worker_num, DefectCenter* defect, const ConfigXML& cfg)
{ 
    _my_rank = my_rank;
    _worker_num = worker_num;
    _defect_center = defect;
    _cfg = cfg;

    if(my_rank == 0)
        _cfg.printParameters();
}

void CCE::run()
{
    set_parameters();
    prepare_center_spin();
    create_bath_spins();
    prepare_bath_state();
    create_spin_clusters();

    run_each_clusters();
    post_treatment();

}

void CCE::prepare_center_spin()
{
    _center_spin = _defect_center->get_espin();
    _state_pair = make_pair( 
            PureState(_defect_center->get_eigen_state(_state_idx0)), 
            PureState(_defect_center->get_eigen_state(_state_idx1)) ); 
}

void CCE::create_bath_spins()
{
    string method = _cfg.getStringParameter("SpinBath", "method");
    if( method.compare("TwoDimLattice") == 0 )
    {
        double lattice_const = _cfg.getDoubleParameter("Lattice", "lattice_const");
        string isotope = _cfg.getStringParameter("Lattice", "isotope");
        int range_i = _cfg.getIntParameter("Lattice", "full_range");
        
        _lattice = TwoDimFaceCenterLattice(lattice_const, isotope);
        _lattice.setRange(range_i);
        
        cSpinSourceFromLattice spin_on_lattice(_lattice);
        _bath_spins = cSpinCollection(&spin_on_lattice);
        _bath_spins.make();
    }
    else
    {
        cSpinSourceFromFile spin_file(_bath_spin_filename);
        _bath_spins = cSpinCollection(&spin_file);
        _bath_spins.make();
        
        if(_my_rank == 0)
            cout << _bath_spins.getSpinNum() << " spins are read from file: " << _bath_spin_filename << endl << endl;
    }
}

void CCE::create_spin_clusters()
{
    string method = _cfg.getStringParameter("SpinBath", "method");

    if( method.compare("TwoDimLattice") == 0 )
    {
        if(_my_rank == 0)
        {
            int root_range = _cfg.getIntParameter("Lattice", "root_range");
            sp_mat c=_bath_spins.getConnectionMatrix(_cut_off_dist);
            cUniformBathOnLattice bath_on_lattice(c,  _max_order, _bath_spins, _lattice, root_range);
            _spin_clusters=cSpinCluster(_bath_spins, &bath_on_lattice);
            _spin_clusters.make();
        }
    }
    else
    {
        if(_my_rank == 0)
        {
            sp_mat c=_bath_spins.getConnectionMatrix(_cut_off_dist);
            cDepthFirstPathTracing dfpt(c, _max_order);
            _spin_clusters=cSpinCluster(_bath_spins, &dfpt);
            _spin_clusters.make();
        }
    }
    
    job_distribution();
}

void CCE::job_distribution()
{/*{{{*/
    uvec clstLength;     vector<umat> clstMat;
    if(_my_rank == 0)
    {
        _spin_clusters.MPI_partition(_worker_num);
        
        clstLength = _spin_clusters.getMPI_ClusterLength(0);
        clstMat = _spin_clusters.getMPI_Cluster(0);
        
        for(int i=1; i<_worker_num; ++i)
        {
            uvec clstNum = _spin_clusters.getMPI_ClusterLength(i);
            MPI_Send(clstNum.memptr(), _max_order, MPI_UNSIGNED, i, 0, MPI_COMM_WORLD);
            
            vector<umat> clstMatList = _spin_clusters.getMPI_Cluster(i);
            for(int j=0; j<_max_order; ++j)
            {
                umat clstMat_j = clstMatList[j];
                MPI_Send(clstMat_j.memptr(), (j+1)*clstNum(j), MPI_UNSIGNED, i, j+1, MPI_COMM_WORLD);
            }
        }
    }
    else
    {
        unsigned int * clstLengthData = new unsigned int [_max_order];
        MPI_Recv(clstLengthData, _max_order, MPI_UNSIGNED, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        
        uvec tempV(clstLengthData, _max_order);
        clstLength = tempV;
        delete [] clstLengthData;
        
        for(int j=0; j<_max_order;++j)
        {
            unsigned int * clstMatData = new unsigned int [(j+1)*clstLength(j)];
            MPI_Recv(clstMatData, (j+1)*clstLength(j), MPI_UNSIGNED, 0, j+1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            umat tempM(clstMatData, clstLength(j), j+1);
            clstMat.push_back(tempM);
            delete [] clstMatData;
        }
    }
    _my_clusters = cSpinCluster(_bath_spins, clstLength, clstMat);
}/*}}}*/

void CCE::run_each_clusters()
{
    for(int cce_order = 0; cce_order < _max_order; ++cce_order)
    {
        cout << "my_rank = " << _my_rank << ": " << "calculating order = " << cce_order << endl;
        size_t clst_num = _my_clusters.getClusterNum(cce_order);
        
        mat resMat(_nTime, clst_num, fill::ones);
        for(int i = 0; i < clst_num; ++i)
        {
            cout << "my_rank = " << _my_rank << ": " << i << "/" << clst_num << endl;
            resMat.col(i) = cluster_evolution(cce_order, i);
        }
        
        DataGathering(resMat, cce_order, clst_num);
    }
}
void CCE::DataGathering(mat& resMat, int cce_order, int clst_num)
{/*{{{*/

    if(_my_rank != 0)
        MPI_Send(resMat.memptr(), _nTime*clst_num, MPI_DOUBLE, 0, 100+_my_rank, MPI_COMM_WORLD);
    else
    {
        double * cce_evolve_data= new double [_nTime * _spin_clusters.getClusterNum(cce_order)];
        memcpy(cce_evolve_data, resMat.memptr(), _nTime*clst_num*sizeof(double));
        
        size_t prev_clst_num = clst_num;
        for(int source = 1; source < _worker_num; ++source)
        {
            pair<size_t, size_t> pos = _spin_clusters.getMPI_ClusterSize(cce_order, source);
            MPI_Recv(cce_evolve_data + _nTime*pos.first, _nTime*(pos.second - pos.first), MPI_DOUBLE, source, 100+source, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }
        
        mat res_i(cce_evolve_data, _nTime, _spin_clusters.getClusterNum(cce_order) );
        _cce_evovle_result.push_back(res_i);

        delete [] cce_evolve_data;
    }
}/*}}}*/

void CCE::post_treatment()
{
    if(_my_rank == 0)
    {
        cce_coherence_reduction();
        compuate_final_coherence();
        export_mat_file();
    }
}

void CCE::cce_coherence_reduction()
{/*{{{*/
    for(int cce_order = 0; cce_order<_max_order; ++cce_order)
    {
        mat tilder_mat =  ones( size(_cce_evovle_result[cce_order]) );
        for(int j = 0; j<_cce_evovle_result[cce_order].n_cols; ++j)
        {
            vec res_j = _cce_evovle_result[cce_order].col(j);
            
            set<ClusterPostion > sub_pos = _spin_clusters.getSubClusters(cce_order, j);
            for(set<ClusterPostion >::iterator it=sub_pos.begin(); it!=sub_pos.end(); ++it)
            {
                vec sub_res = _cce_evovle_result_tilder[it->first].col(it->second);
                res_j = res_j / sub_res;
            }
            tilder_mat.col(j) = res_j;
        }
        _cce_evovle_result_tilder.push_back( tilder_mat );
    }

}/*}}}*/

void CCE::compuate_final_coherence()
{/*{{{*/
    _final_result = mat(_nTime, _max_order, fill::zeros);
    _final_result_each_order = mat(_nTime, _max_order, fill::zeros);

    vec final_res_vec = ones<vec> (_nTime);
    for(int cce_order = 0; cce_order<_max_order; ++cce_order)
    {
        vec res_vec = ones<vec> (_nTime);
        for(int j=0; j<_cce_evovle_result_tilder[cce_order].n_cols; ++j)
            res_vec = res_vec % _cce_evovle_result_tilder[cce_order].col(j);
        _final_result_each_order.col(cce_order) = res_vec;
        
        final_res_vec = final_res_vec % res_vec;
        _final_result.col(cce_order)= final_res_vec;
    }
}/*}}}*/

void CCE::export_mat_file() 
{/*{{{*/
#ifdef HAS_MATLAB
    cout << "begin post_treatement ... storing cce_data to file: " << _result_filename << endl;
    MATFile *mFile = matOpen(_result_filename.c_str(), "w");
    for(int i=0; i<_max_order; ++i)
    {
        char i_str [10];
        sprintf(i_str, "%d", i);
        string idx_str = i_str;
        string label = "CCE" + idx_str;
        string label1 = "CCE" + idx_str+"_tilder";
        
        size_t nClst = _spin_clusters.getClusterNum(i);
        mxArray *pArray = mxCreateDoubleMatrix(_nTime, nClst, mxREAL);
        mxArray *pArray1 = mxCreateDoubleMatrix(_nTime, nClst, mxREAL);
        
        size_t length= _nTime * nClst;
        memcpy((void *)(mxGetPr(pArray)), (void *) _cce_evovle_result[i].memptr(), length*sizeof(double));
        memcpy((void *)(mxGetPr(pArray1)), (void *) _cce_evovle_result_tilder[i].memptr(), length*sizeof(double));
        
        matPutVariableAsGlobal(mFile, label.c_str(), pArray);
        matPutVariableAsGlobal(mFile, label1.c_str(), pArray1);
        
        mxDestroyArray(pArray);
        mxDestroyArray(pArray1);
    }

    mxArray *pRes = mxCreateDoubleMatrix(_nTime, _max_order, mxREAL);
    mxArray *pRes1 = mxCreateDoubleMatrix(_nTime, _max_order, mxREAL);
    mxArray *pTime = mxCreateDoubleMatrix(_nTime, 1, mxREAL);
    size_t length= _nTime*_max_order;
    memcpy((void *)(mxGetPr(pRes)), (void *) _final_result_each_order.memptr(), length*sizeof(double));
    memcpy((void *)(mxGetPr(pRes1)), (void *) _final_result.memptr(), length*sizeof(double));
    memcpy((void *)(mxGetPr(pTime)), (void *) _time_list.memptr(), _nTime*sizeof(double));
    matPutVariableAsGlobal(mFile, "final_result_each_order", pRes);
    matPutVariableAsGlobal(mFile, "final_result", pRes1);
    matPutVariableAsGlobal(mFile, "time_list", pTime);
    mxDestroyArray(pRes);
    mxDestroyArray(pRes1);
    mxDestroyArray(pTime);
    matClose(mFile);
#endif
}/*}}}*/
//}}}
////////////////////////////////////////////////////////////////////////////////



////////////////////////////////////////////////////////////////////////////////
//{{{  EnsembleCCE
void EnsembleCCE::set_parameters()
{/*{{{*/
    string input_filename  = _cfg.getStringParameter("Data",       "input_file");
    string output_filename = _cfg.getStringParameter("Data",       "output_file");

    _state_idx0            = _cfg.getIntParameter   ("CenterSpin", "state_index0");
    _state_idx1            = _cfg.getIntParameter   ("CenterSpin", "state_index1");

    _center_spin_name      = _cfg.getStringParameter("CenterSpin", "name");
    _cut_off_dist          = _cfg.getDoubleParameter("SpinBath",   "cut_off_dist");
    _max_order             = _cfg.getIntParameter   ("CCE",        "max_order");
    _nTime                 = _cfg.getIntParameter   ("Dynamics",   "nTime");
    _t0                    = _cfg.getDoubleParameter("Dynamics",   "t0"); 
    _t1                    = _cfg.getDoubleParameter("Dynamics",   "t1"); 
    _pulse_name            = _cfg.getStringParameter("Condition",  "pulse_name");
    _pulse_num             = _cfg.getIntParameter   ("Condition",  "pulse_number");

    _magB << _cfg.getDoubleParameter("Condition",  "magnetic_fieldX")
           << _cfg.getDoubleParameter("Condition",  "magnetic_fieldY")
           << _cfg.getDoubleParameter("Condition",  "magnetic_fieldZ");

    _bath_spin_filename = INPUT_PATH + input_filename;
    _result_filename    = OUTPUT_PATH + output_filename;

    _time_list = linspace<vec>(_t0, _t1, _nTime);
}/*}}}*/


void EnsembleCCE::prepare_bath_state()
{
    _bath_polarization = zeros<vec>(3);
}

vec EnsembleCCE::cluster_evolution(int cce_order, int index)
{
    vector<cSPIN> spin_list = _my_clusters.getCluster(cce_order, index);
    
    Hamiltonian hami0 = create_spin_hamiltonian(_center_spin, _state_pair.first, spin_list);
    Hamiltonian hami1 = create_spin_hamiltonian(_center_spin, _state_pair.second, spin_list);
    
    vector<QuantumOperator> left_hm_list = riffle((QuantumOperator) hami0, (QuantumOperator) hami1, _pulse_num);
    vector<QuantumOperator> right_hm_list;
    if (_pulse_num % 2 == 0)
        right_hm_list= riffle((QuantumOperator) hami1, (QuantumOperator) hami0, _pulse_num);
    else
        right_hm_list = riffle((QuantumOperator) hami0, (QuantumOperator) hami1, _pulse_num);

    vector<double> time_segment = Pulse_Interval(_pulse_name, _pulse_num);

    DensityOperator ds = create_spin_density_state(spin_list);

    PiecewiseFullMatrixMatrixEvolution kernel(left_hm_list, right_hm_list, time_segment, ds);
    kernel.setTimeSequence( _t0, _t1, _nTime);

    ClusterCoherenceEvolution dynamics(&kernel);
    dynamics.run();
    
    return calc_observables(&kernel);
}

Hamiltonian EnsembleCCE::create_spin_hamiltonian(const cSPIN& espin, const PureState& center_spin_state, const vector<cSPIN>& spin_list)
{
    SpinDipolarInteraction dip(spin_list);

    SpinZeemanInteraction zee(spin_list, _magB);

    DipolarField hf_field(spin_list, espin, center_spin_state);

    Hamiltonian hami(spin_list);
    hami.addInteraction(dip);
    hami.addInteraction(zee);
    hami.addInteraction(hf_field);
    hami.make();
    return hami;
}

Liouvillian EnsembleCCE::create_spin_liouvillian(const Hamiltonian& hami0, const Hamiltonian hami1)
{
    Liouvillian lv0(hami0, SHARP);
    Liouvillian lv1(hami1, FLAT);
    Liouvillian lv = lv0 - lv1;
    return lv;
}

DensityOperator EnsembleCCE::create_spin_density_state(const vector<cSPIN>& spin_list)
{
    SpinPolarization p(spin_list, _bath_polarization);

    DensityOperator ds(spin_list);
    ds.addStateComponent(p);
    ds.make();
    ds.makeVector();
    return ds;
}

vec EnsembleCCE::calc_observables(QuantumEvolutionAlgorithm* kernel)
{
    vector<cx_mat>  state = kernel->getResultMat();
    vec res = ones<vec>(_nTime);
    for(int i=0; i<_nTime; ++i)
        res(i) = real( trace(state[i]) );
    return res;
}
//}}}
////////////////////////////////////////////////////////////////////////////////




////////////////////////////////////////////////////////////////////////////////
//{{{  SingleSampleCCE
void SingleSampleCCE::set_parameters()
{/*{{{*/
    string input_filename  = _cfg.getStringParameter("Data",       "input_file");
    string output_filename = _cfg.getStringParameter("Data",       "output_file");

    _state_idx0            = _cfg.getIntParameter   ("CenterSpin", "state_index0");
    _state_idx1            = _cfg.getIntParameter   ("CenterSpin", "state_index1");
    
    _center_spin_name      = _cfg.getStringParameter("CenterSpin", "name");
    _cut_off_dist          = _cfg.getDoubleParameter("SpinBath",   "cut_off_dist");
    _bath_state_seed       = _cfg.getIntParameter   ("SpinBath", "bath_state_seed");
    _max_order             = _cfg.getIntParameter   ("CCE",        "max_order");
    _nTime                 = _cfg.getIntParameter   ("Dynamics",   "nTime");
    _t0                    = _cfg.getDoubleParameter("Dynamics",   "t0"); 
    _t1                    = _cfg.getDoubleParameter("Dynamics",   "t1"); 
    _pulse_name            = _cfg.getStringParameter("Condition",  "pulse_name");
    _pulse_num             = _cfg.getIntParameter   ("Condition",  "pulse_number");

    _magB << _cfg.getDoubleParameter("Condition",  "magnetic_fieldX")
           << _cfg.getDoubleParameter("Condition",  "magnetic_fieldY")
           << _cfg.getDoubleParameter("Condition",  "magnetic_fieldZ");

    _bath_spin_filename = INPUT_PATH + input_filename;
    _result_filename    = OUTPUT_PATH + output_filename;

    _time_list = linspace<vec>(_t0, _t1, _nTime);
}/*}}}*/

void SingleSampleCCE::prepare_bath_state()
{/*{{{*/
    vector<cSPIN> sl = _bath_spins.getSpinList();
    srand(_bath_state_seed);
    for(int i=0; i<sl.size(); ++i)
    {
        PureState psi_i(sl[i]);
        psi_i.setComponent( rand()%2, 1.0);
        _bath_state_list.push_back(psi_i);
    }

    //cache_dipole_field();
}/*}}}*/

vec SingleSampleCCE::cluster_evolution(int cce_order, int index)
{/*{{{*/
    vector<cSPIN> spin_list = _my_clusters.getCluster(cce_order, index);
    cClusterIndex clstIndex = _my_clusters.getClusterIndex(cce_order, index);

    Hamiltonian hami0 = create_spin_hamiltonian(_center_spin, _state_pair.first, spin_list, clstIndex);
    Hamiltonian hami1 = create_spin_hamiltonian(_center_spin, _state_pair.second, spin_list, clstIndex);

    vector<QuantumOperator> hm_list1 = riffle((QuantumOperator) hami0, (QuantumOperator) hami1, _pulse_num);
    vector<QuantumOperator> hm_list2 = riffle((QuantumOperator) hami1, (QuantumOperator) hami0, _pulse_num);
    vector<double> time_segment = Pulse_Interval(_pulse_name, _pulse_num);

    PureState psi = create_cluster_state(clstIndex);

    PiecewiseFullMatrixVectorEvolution kernel1(hm_list1, time_segment, psi);
    PiecewiseFullMatrixVectorEvolution kernel2(hm_list2, time_segment, psi);
    kernel1.setTimeSequence( _t0, _t1, _nTime);
    kernel2.setTimeSequence( _t0, _t1, _nTime);

    ClusterCoherenceEvolution dynamics1(&kernel1);
    ClusterCoherenceEvolution dynamics2(&kernel2);
    dynamics1.run();
    dynamics2.run();

    return calc_observables(&kernel1, &kernel2);
}/*}}}*/

Hamiltonian SingleSampleCCE::create_spin_hamiltonian(const cSPIN& espin, const PureState& center_spin_state, const vector<cSPIN>& spin_list, const cClusterIndex& clstIndex )
{/*{{{*/
    SpinDipolarInteraction dip(spin_list);

    SpinZeemanInteraction zee(spin_list, _magB);

    DipolarField hf_field(spin_list, espin, center_spin_state);

    DipolarField bath_field(spin_list, _bath_spins.getSpinList(), _bath_state_list, clstIndex.getIndex() );

    Hamiltonian hami(spin_list);
    hami.addInteraction(dip);
    hami.addInteraction(zee);
    hami.addInteraction(hf_field);
    hami.addInteraction(bath_field);
    hami.make();
    return hami;
}/*}}}*/

Liouvillian SingleSampleCCE::create_spin_liouvillian(const Hamiltonian& hami0, const Hamiltonian hami1)
{/*{{{*/
    Liouvillian lv0(hami0, SHARP);
    Liouvillian lv1(hami1, FLAT);
    Liouvillian lv = lv0 - lv1;
    return lv;
}/*}}}*/

PureState SingleSampleCCE::create_cluster_state(const cClusterIndex& clstIndex)
{/*{{{*/
    uvec idx = clstIndex.getIndex();
    cx_vec state_vec = _bath_state_list[ idx[0] ].getVector();
    for(int i=1; i<idx.n_elem; ++i)
        state_vec = kron( state_vec, _bath_state_list[ idx[i] ].getVector() );

    PureState res( state_vec );
    return res;
}/*}}}*/

void SingleSampleCCE::cache_dipole_field()
{/*{{{*/
    vector<cSPIN> sl = _bath_spins.getSpinList();
    for(int i=0; i<sl.size(); ++i)
    {
        vector<vec> dip_i;
        for(int j=0; j<sl.size(); ++j)
        {
            vec v_i = dipole_field(sl[i], sl[j], _bath_state_list[j].getVector() ); 
            dip_i.push_back( v_i );
        }
        _dipole_field_data.push_back(dip_i);
    }
}/*}}}*/

vec SingleSampleCCE::calc_observables(QuantumEvolutionAlgorithm* kernel1, QuantumEvolutionAlgorithm* kernel2)
{/*{{{*/
    vector<cx_vec>  state1 = kernel1->getResult();
    vector<cx_vec>  state2 = kernel2->getResult();
    cx_vec res = ones<cx_vec>(_nTime);
    for(int i=0; i<_nTime; ++i)
        res(i) =  cdot(state1[i], state2[i]);
    return real(res);
}/*}}}*/
//}}}
////////////////////////////////////////////////////////////////////////////////
