#include "include/app/app.h"
#include "include/math/MatExp.h"

void test_small_mat();
void test_large_mat();
void test_very_large_mat();

int  main(int argc, char* argv[])
{

    //test_small_mat();
    test_large_mat();
    //test_very_large_mat();
    return 0;
}

void test_small_mat()
{/*{{{*/
    cx_double II = cx_double(0.0, 1.0);

    cx_mat H; 
    H << 6.0        << 1.0 + 1.0*II << endr
      << 1.0-1.0*II << 3.0;

    cx_double prefactor = 300.0;

    cout << "input data: " << endl;
    cout << II*prefactor * H << endl;
    cx_mat resArma, resPade;
    
    MatExp expM(H, II*prefactor, MatExp::ArmadilloExpMat);
    expM.run();
    resArma = expM.getResultMatrix();
    cout << "arma output: " << endl;
    cout << resArma << endl;
    
    MatExp expM2(H, II*prefactor, MatExp::PadeApproximation);
    expM2.run();

    resPade = expM2.getResultMatrix();
    cout << "pade output: " << endl;
    cout << resPade << endl;

    cout << "diff" << endl;
    cout << resArma-resPade << endl; 
}/*}}}*/

void test_large_mat()
{
    //please run this application in "oops/" direcotry 
    cSpinSourceFromFile spin_file("./dat/input/RoyCoord.xyz4");
    cSpinCollection spins(&spin_file);
    spins.make();

    vector<cSPIN> sl = spins.getSpinList();

    SpinDipolarInteraction dip(sl);
    Hamiltonian hami(sl);
    hami.addInteraction(dip);
    hami.make();

    cx_mat H = hami.getMatrix(); 
    int dim = H.n_cols;


    PureState psi(dim);
    psi.setComponent(0, 1.0);
    cx_vec v = psi.getVector();

    std::complex<double> * mat = H.memptr();
    std::complex<double> * vecC = v.memptr();

    cout << "mat = " << endl;
    for(int i=0; i<dim; ++i)
    {
        for(int j=0; j<dim; ++j)
            cout << mat[i*dim+j] << endl;
        cout << endl;
    }
    cout << "vector = " << endl;
    for(int i=0; i<dim; ++i)
        cout << vecC[i] << endl;

}

void test_very_large_mat()
{/*{{{*/
    //please run this application in "oops/" direcotry 
    cSpinSourceFromFile spin_file("./dat/input/RoyCoord.xyz4");
    cSpinCollection spins(&spin_file);
    spins.make();

    vector<cSPIN> sl = spins.getSpinList();

    SpinDipolarInteraction dip(sl);
    Hamiltonian hami(sl);
    hami.addInteraction(dip);
    hami.make();

    SumKronProd skp = hami.getKronProdForm();

    PureState psi( skp.getDim() );
    psi.setComponent(0, 1.0);
    cout << psi.getVector() << endl;

    vec time_list = linspace<vec>(0, 1, 11);
    MatExpVector expM(hami.getKronProdForm(), -1.0*II, psi.getVector(), time_list);  
    MatExpVector expMgpu(hami.getKronProdForm(), -1.0*II, psi.getVector(), time_list);  
    expM.run();
    expMgpu.run_gpu();

    cx_mat res = expM.getResult();
    cx_mat resGPU = expMgpu.getResult();
    cout << res << endl;
    cout << res - resGPU << endl;

    cx_mat H = hami.getMatrix(); 
    MatExp expM2(H, -1.0*II, MatExp::PadeApproximation);
    expM2.run();
    cx_mat resPade = expM2.getResultMatrix();

    cout << resPade*psi.getVector() << endl;
}/*}}}*/
