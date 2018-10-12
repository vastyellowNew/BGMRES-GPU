#include <Teuchos_ScalarTraits.hpp>
#include <Teuchos_RCP.hpp>
#include <Teuchos_oblackholestream.hpp>
#include <Teuchos_VerboseObject.hpp>
#include <Teuchos_CommandLineProcessor.hpp>
#include <Teuchos_GlobalMPISession.hpp>

#include <Tpetra_Core.hpp>
#include <Tpetra_Map.hpp>
#include <Tpetra_MultiVector.hpp>
#include <Tpetra_CrsMatrix.hpp>
#include <Tpetra_DefaultPlatform.hpp>

#include "BelosConfigDefs.hpp"
#include "BelosLinearProblem.hpp"
#include "BelosTpetraAdapter.hpp"

#include "BelosBlockGmresSolMgr.hpp"

// I/O for Matrix-Market files
#include <MatrixMarket_Tpetra.hpp>
#include <Tpetra_Import.hpp>

// SMG2S test matrix generation with given spectra
#include "SMG2S/smg2s/smg2s.h"

// SMG2S interface to Trilinos/teptra csr sparse matrix
#include "SMG2S/interface/Trilinos/trilinos_cuda_interface.hpp"

#include <ctime>

typedef std::complex<double>                  					ST;


typedef Teuchos::ScalarTraits<ST>             					SCT;
typedef SCT::magnitudeType                    					MT;


typedef Kokkos::Compat::KokkosDeviceWrapperNode<Kokkos::Cuda> node_type;


typedef Tpetra::Map<>::local_ordinal_type     					LO;
typedef Tpetra::Map<>::global_ordinal_type    					GO;
typedef Tpetra::Operator<ST,int>              					OP;
typedef Tpetra::CrsMatrix<ST,LO,GO,node_type>           		MAT;
typedef Tpetra::MultiVector<ST,LO,GO>         					MV;
typedef Belos::MultiVecTraits<ST,MV>          					MVT;
typedef Belos::OperatorTraits<ST,MV,OP>       					OPT;

int main(int argc, char *argv[]){

	Teuchos::GlobalMPISession mpisess (&argc, &argv, &std::cout);

	const ST ONE  = SCT::one();

	using Tpetra::global_size_t;
	using Tpetra::Map;
	using Tpetra::Import;
	using Teuchos::RCP;
	using Teuchos::rcp;

	//
	// Get the default communicator
	//
	Teuchos::RCP<const Teuchos::Comm<int> > comm = Tpetra::getDefaultComm();
	int myRank = comm->getRank();

    int grank, gsize;
    int type = 666;
    int exit_type = 0;


    MPI_Comm_size( MPI_COMM_WORLD, &gsize );
    MPI_Comm_rank( MPI_COMM_WORLD, &grank );

	Teuchos::oblackholestream blackhole;

	bool printMatrix   = false;
	bool allprint      = false;
	bool verbose 	   = (myRank==0);
	bool debug 		   = false;
	std::string filename("utm300_cp.mtx");
	int frequency 	   = -1;
	int numVectors 	   = 2;
  int blocksize 	   = 100;
	int numblocks 	   = 50;
	double tol 		   = 1.0e-5;
	bool precond 	   = false;
	bool dumpdata 	   = false;
	bool reduce_tol;
	Teuchos::ParameterList mptestpl;
	Belos::ReturnType ret;
	double t1, t2, t3, t4, t5, t6;

	int lsPower = 10;
	int latency = 1;
	bool lspuse = true;

	int max_iters = 5000;
	int max_restarts = 1000;

	/*smg2s parameters*/
	bool usesmg2s = false;
	global_ordinal_type probSize = 10;
	int continous = 4;
	int lbandwidth = 4;
	std::string spectra_file = " ";

	Teuchos::CommandLineProcessor cmdp(false,true);
	cmdp.setOption("ksp-verbose","ksp-quiet",&verbose,"Print messages and results.");
	cmdp.setOption("ksp-debug","ksp-nodebug",&debug,"Run debugging checks.");
	cmdp.setOption("ksp-frequency",&frequency,"Solvers frequency for printing residuals (#iters).");
	cmdp.setOption("ksp-tol",&tol,"Relative residual tolerance used by solver.");
	cmdp.setOption("ksp-num-rhs",&numVectors,"Number of right-hand sides to be solved for.");
	cmdp.setOption("ksp-block-size",&blocksize,"Block size to be used by the solver.");
	cmdp.setOption("ksp-use-precond","ksp-no-precond",&precond,"Use a diagonal preconditioner.");
	cmdp.setOption("ksp-num-blocks",&numblocks,"Number of blocks in the Krylov basis.");
	cmdp.setOption("ksp-reduce-tol","ksp-fixed-tol",&reduce_tol,"Require increased accuracy from higher precision scalar types.");
	cmdp.setOption("ksp-filename",&filename,"Filename for Matrix-Market test matrix.");
	cmdp.setOption("ksp-print-matrix","ksp-no-print-matrix",&printMatrix,"Print the full matrix after reading it.");
	cmdp.setOption("ksp-all-print","ksp-root-print",&allprint,"All processors print to out");
	cmdp.setOption("ksp-dump-data","ksp-no-dump-data",&dumpdata,"Dump raw data to data.dat.");

	/*smg2s options*/
	cmdp.setOption("ksp-usesmg2s","ksp-no-usesmg2s",&usesmg2s,"Use SMG2S to generate test matrix.");
	cmdp.setOption("ksp-smg2s-size",&probSize,"Dimension of matrix generated by SMG2S.");
	cmdp.setOption("ksp-smg2s-continous",&continous,"Number of continous 1 of nilpotent matrix in SMG2S.");
	cmdp.setOption("ksp-smg2s-lbandwidth",&lbandwidth,"Low bandwidth of initial matrix in SMG2S.");
	cmdp.setOption("ksp-smg2s-spectra-file",&spectra_file,"Given spectra filename in SMG2S.");


	cmdp.setOption("ksp-lsp-degree",&lsPower,"Least Square polynomial degree for preconditioning.");
    cmdp.setOption("ksp-lsp-latency",&latency,"Latency of Least Square polynomial preconditioning to apply.");
	cmdp.setOption("ksp-use-lsp","ksp-no-use-lsp",&lspuse, "Whether to use LS polynomial preconditioning.");

	if (cmdp.parse(argc,argv) != Teuchos::CommandLineProcessor::PARSE_SUCCESSFUL) {
  	return -1;
	}

	if (debug) {
  	verbose = true;
	}

	if (!verbose) {
  	frequency = -1;
	}

	mptestpl.set( "Block Size", blocksize );
 	mptestpl.set( "Num Blocks", numblocks);
	
	mptestpl.set("LS Polynomial Degree", lsPower);
    mptestpl.set("LS Apply Latency", latency);
    mptestpl.set("LS USE", lspuse);

	mptestpl.set<MT>( "Convergence Tolerance", tol );
	mptestpl.set( "Timer Label",  Teuchos::typeName(ONE) );

	mptestpl.set("Maximum Iterations", max_iters);
	mptestpl.set("Maximum Restarts", max_restarts);

	int verbLevel = Belos::Errors + Belos::Warnings;
/*
	if (debug) {
    	verbLevel |= Debug;
  	}
*/
	if (verbose) {
  	verbLevel |= Belos::TimingDetails + Belos::FinalSummary + Belos::StatusTestDetails;
	}
	mptestpl.set( "Verbosity", verbLevel );
	if (verbose) {
  	if (frequency > 0) {
    		mptestpl.set( "Output Frequency", frequency );
  	}
	}

	std::ostream& out = ( (allprint || (myRank == 0)) ? std::cout : blackhole );
	RCP<Teuchos::FancyOStream> fos = Teuchos::fancyOStream(Teuchos::rcpFromRef(out));

	if (myRank==0){
		std::cout << "Testing Scalar == " << Teuchos::typeName(ONE) << std::endl;
	}

	/*build the problem by loading matrix market from local file systems*/
	if (myRank==0){
		std::cout << "GMRES ]> Building problem..." << std::endl;
	}
  	
  	
	//	Teuchos::TimeMonitor localtimer(btimer); 
	t1 = MPI_Wtime();

	RCP<MAT> A;
	
	if(usesmg2s){
	/*Construct test matrix by SMG2S (scalable matrix generator with given spectrum) */
		Nilpotency<global_ordinal_type> nilp;

		nilp.NilpType1(continous,probSize);

		parMatrixSparse<ST,global_ordinal_type> *Mt;

		Mt = smg2s<ST,global_ordinal_type> (probSize, nilp,lbandwidth, spectra_file, MPI_COMM_WORLD);

		A = ConvertToTrilinosCudaMat(Mt);

		A->fillComplete ();

	    if( printMatrix ){
	      A->describe(*fos, Teuchos::VERB_EXTREME);
	    } else if( verbose ){
	      std::cout << std::endl << A->description() << std::endl << std::endl;
	    }

	    if (myRank == 0){
	      std::cout << "GMRES ]> SMG2S generated test matrix ... " << Teuchos::typeName(ONE) << std::endl;
	    }
	  }else{
	    A = Tpetra::MatrixMarket::Reader<MAT>::readSparseFile(filename, comm);

	    if (myRank == 0){
	      std::cout << "GMRES ]> Matrix Loaded ... " << Teuchos::typeName(ONE) << std::endl;
	    }

	    if( printMatrix ){
	      A->describe(*fos, Teuchos::VERB_EXTREME);
	    } else if( verbose ){
	      std::cout << std::endl << A->description() << std::endl << std::endl;
	    }
	}

	// get the maps
	RCP<const Map<LO,GO> > dmnmap = A->getDomainMap();
	RCP<const Map<LO,GO> > rngmap = A->getRangeMap();

	GO nrows = dmnmap->getGlobalNumElements();
	RCP<Map<LO,GO> > root_map
  	= rcp( new Map<LO,GO>(nrows,myRank == 0 ? nrows : 0,0,comm) );
	RCP<MV> Xhat = rcp( new MV(root_map,numVectors) );
	RCP<Import<LO,GO> > importer = rcp( new Import<LO,GO>(dmnmap,root_map) );

	RCP<MV> X = rcp(new MV(dmnmap,numVectors));

	ST v;
	double rnd_r, rnd_i;
	GO i; 
	int j;
	for(j = 0; j < numVectors; j++){
		std::srand((i + j)*1.2345);
		for(i = 0; i < nrows; i++){
			rnd_r = (double)std::rand()/RAND_MAX;
			rnd_i = (double)std::rand()/RAND_MAX;
			v.real(rnd_r);
			v.imag(rnd_i);
		}		
	}


	// generate randomly the final solution of systems as X
//	RCP<MV> X = rcp(new MV(dmnmap,numVectors));
	//X->randomize();
	MVT::MvRandom(*X);
	//X->describe(*fos, Teuchos::VERB_EXTREME);

	/*generate the right hand sides B by given X and A */
	RCP<MV> B = rcp(new MV(dmnmap,numVectors));
	OPT::Apply( *A, *X, *B );
	MVT::MvInit( *X, 0.0 );

	RCP<Belos::LinearProblem<ST,MV,OP> > problem = rcp( new Belos::LinearProblem<ST,MV,OP>(A,X,B) );

	problem->setLabel(Teuchos::typeName(SCT::one()));

	TEUCHOS_TEST_FOR_EXCEPT(problem->setProblem() == false);



  	t2 = MPI_Wtime();

	if(myRank == 0) printf("Building TIME = %f\n", t2 - t1 ); 
 	
	/////////////////////////////////////////////////
	/*construct GMRES solver*/


	RCP<Belos::SolverManager<ST,MV,OP> > solver;
	if (myRank==0){
  		std::cout << "GMRES ]> Construct solver ..." << std::endl;
  	}
  	
	t3 = MPI_Wtime();
	//Teuchos::TimeMonitor localtimer(ctimer); 
	solver = rcp(new Belos::BlockGmresSolMgr<ST,MV,OP>( problem, rcp(&mptestpl,false) ));
	t4 = MPI_Wtime();
	
	if(myRank == 0) printf("Construct TIME = %f\n", t4 - t3 ); 

	if(myRank == 0) printf("GMRES ]> Solving problem ...\n" ); 

	int diter;

	t5 = MPI_Wtime();
	try {
      ret = solver->solve();
    }

    catch (std::exception &e) {
      std::cout << "Caught exception: " << std::endl << e.what() << std::endl;
      ret = Belos::Unconverged;
    }

  	t6 = MPI_Wtime();

	if(myRank == 0) printf("Solving TIME = %f\n", t6 - t5 ); 

	diter = solver->getNumIters();

	if (ret == Belos::Converged){
		if(myRank == 0){
			printf("GMRES ]> This GMRES Component is converged\n");
		}
	}
	else{
		if(myRank == 0){
			printf("GMRES ]> This GMRES Component cannot be converged with given parameters\n");
		}
	}
  
  	printf("GMRES ]> Close of GMRES after waiting a little instant\n");


	return 0;


}
