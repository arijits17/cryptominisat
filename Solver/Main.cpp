/*
 * CryptoMiniSat
 *
 * Copyright (c) 2009-2011, Mate Soos and collaborators. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3.0 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301  USA
*/

/**
@mainpage CryptoMiniSat
@author Mate Soos, and collaborators

CryptoMiniSat is an award-winning SAT solver based on MiniSat. It brings a
number of benefits relative to MiniSat.
*/

#include <ctime>
#include <cstring>
#include <errno.h>
#include <string.h>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <set>
#include <signal.h>

#include "Main.h"
#include "constants.h"
#include "time_mem.h"
#include "constants.h"
#include "DimacsParser.h"
#include "ThreadControl.h"

#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>
using boost::lexical_cast;
namespace po = boost::program_options;

Main::Main(int _argc, char** _argv) :
        numThreads(1)
        , debugLib (false)
        , debugNewVar (false)
        , printResult (true)
        , max_nr_of_solutions (1)
        , doBanFoundSolution(true)
        , fileNamePresent (false)
        , argc(_argc)
        , argv(_argv)
{
}

ThreadControl* solverToInterrupt;

/**
@brief For correctly and gracefully exiting

It can happen that the user requests a dump of the learnt clauses. In this case,
the program must wait until it gets to a state where the learnt clauses are in
a correct state, then dump these and quit normally. This interrupt hander
is used to achieve this
*/
void SIGINT_handler(int)
{
    ThreadControl* control = solverToInterrupt;
    std::cout << "c " << std::endl;
    std::cerr << "*** INTERRUPTED ***" << std::endl;
    if (control->getNeedToDumpLearnts() || control->getNeedToDumpOrig()) {
        control->setNeedToInterrupt();
        std::cerr << "*** Please wait. We need to interrupt cleanly" << std::endl;
        std::cerr << "*** This means we might need to finish some calculations" << std::endl;
    } else {
        if (control->getVerbosity() >= 1)
            control->printStats();
        _exit(1);
    }
}

void Main::readInAFile(const std::string& filename)
{
    if (conf.verbosity >= 1) {
        std::cout << "c Reading file '" << filename << "'" << std::endl;
    }
    #ifdef DISABLE_ZLIB
        FILE * in = fopen(filename.c_str(), "rb");
    #else
        gzFile in = gzopen(filename.c_str(), "rb");
    #endif // DISABLE_ZLIB

    if (in == NULL) {
        std::cout << "ERROR! Could not open file '" << filename << "' for reading" << std::endl;
        exit(1);
    }

    DimacsParser parser(control, debugLib, debugNewVar);
    parser.parse_DIMACS(in);

    #ifdef DISABLE_ZLIB
        fclose(in);
    #else
        gzclose(in);
    #endif // DISABLE_ZLIB
}

void Main::readInStandardInput()
{
    if (control->getVerbosity()) {
        std::cout << "c Reading from standard input... Use '-h' or '--help' for help." << std::endl;
    }
    #ifdef DISABLE_ZLIB
        FILE * in = stdin;
    #else
        gzFile in = gzdopen(fileno(stdin), "rb");
    #endif // DISABLE_ZLIB

    if (in == NULL) {
        std::cout << "ERROR! Could not open standard input for reading" << std::endl;
        exit(1);
    }

    DimacsParser parser(control, debugLib, debugNewVar);
    parser.parse_DIMACS(in);

    #ifndef DISABLE_ZLIB
        gzclose(in);
    #endif // DISABLE_ZLIB
}

void Main::parseInAllFiles()
{
    const double myTime = cpuTime();

    //First read normal extra files
    if ((debugLib || debugNewVar) && filesToRead.size() > 0) {
        std::cout << "debugNewVar and debugLib must both be OFF to parse in extra files" << std::endl;
        exit(-1);
    }

    for (vector<string>::const_iterator it = filesToRead.begin(), end = filesToRead.end(); it != end; it++) {
        readInAFile(it->c_str());
    }

    if (!fileNamePresent)
        readInStandardInput();

    if (conf.verbosity >= 1) {
        std::cout << "c Parsing time: "
        << std::fixed << std::setw(5) << std::setprecision(2) << (cpuTime() - myTime)
        << " s" << std::endl;
    }
}

void Main::printResultFunc(const lbool ret)
{
    if (ret == l_True) {
        if (!printResult) std::cout << "c SATISFIABLE" << std::endl;
        else              std::cout << "s SATISFIABLE" << std::endl;
    } else if (ret == l_False) {
        if (!printResult) std::cout << "c UNSATISFIABLE" << std::endl;
        else              std::cout << "s UNSATISFIABLE" << std::endl;
    }

    if(ret == l_True && printResult) {
        std::stringstream toPrint;
        toPrint << "v ";
        for (Var var = 0; var != control->nVars(); var++)
            if (control->model[var] != l_Undef)
                toPrint << ((control->model[var] == l_True)? "" : "-") << var+1 << " ";
            toPrint << "0" << std::endl;
        std::cout << toPrint.str();
    }
}

struct WrongParam
{
    WrongParam(std::string _param, std::string _msg) :
        param(_param)
        , msg(_msg)
    {}

    const std::string& getMsg() const
    {
        return msg;
    }

    const std::string& getParam() const
    {
        return param;
    }

    std::string param;
    std::string msg;
};

void Main::parseCommandLine()
{
    conf.verbosity = 2;

    // Declare the supported options.
    po::options_description generalOptions("Most important options");
    generalOptions.add_options()
    ("help,h", "Prints this help")
    ("input", po::value< std::vector<std::string> >(), "file(s) to read")
    ("polar", po::value<std::string>()->default_value("auto"), "{true,false,rnd,auto} Selects polarity mode")
    //("  -decay", " <num> [ 0 - 1 ]")
    ("freq", po::value<double>()->default_value(conf.random_var_freq), "[0 - 1] freq. of picking var at random")
    ("verbosity", po::value<int>()->default_value(conf.verbosity), "[0-4] Verbosity of solver")
    ("randomize", po::value<uint32_t>()->default_value(conf.origSeed), "[0..] Sets random seed")
    ("restart", po::value<std::string>()->default_value("auto"), "{auto,static,dynamic}  Restart strategy to follow.")
    ("threads,t", po::value<uint32_t>()->default_value(numThreads), "Threads to use")
    ("nosolprint", "Don't print assignment if solution is SAT")
    ("nosimplify", "Don't do regular simplification rounds")
    //("greedyunbound", "Greedily unbound variables that are not needed for SAT")
    ;
    //("pavgbranch", "Print average branch depth")

    po::options_description iterativeOptions("Iterative solve options");
    iterativeOptions.add_options()
    ("maxsolutions", "Search for given amount of solutions")
    ("dumplearnts", po::value<std::string>(), "If stopped dump learnt clauses here")
    ("maxdump", po::value<uint32_t>(), "Maximum length of learnt clause dumped")
    ("dumporig", po::value<std::string>(), "If stopped, dump simplified original problem here")
    ("nobanfoundsol", "Don't ban solutions found")
    ("debuglib", "Solve at specific 'solve()' points in CNF file")
    ("debugnewvar", "Add new vars at specific 'newVar()' points in 6CNF file")
    ;

    po::options_description failedLitOptions("Failed lit options");
    failedLitOptions.add_options()
    ("nofailedlit", "Don't do failed literals at ALL (none below)")
    ("nohyperbinres", "Don't add binary clauses when doing failed lit probing.")
    ("noremovebins", "Don't remove useless binary clauses")
    ;

    po::options_description sateliteOptions("Normal satelite-type options");
    sateliteOptions.add_options()
    ("nosatelite", "Don't play with norm clauses at ALL (none below)")
    ("novarelim", "Don't perform variable elimination as per Een and Biere")
    ("nosubsume1", "Don't perform clause contraction through resolution")
    ("noblocked", "No blocked-clause removal")
    ;

    po::options_description xorSateliteOptions("XOR satelite-type options");
    xorSateliteOptions.add_options()
    ("noxorsatelite", "Don't play with xor-clauses at ALL (none below)")
    ("noconglomerate", "Don't eliminate var by xoring 2 xors")
    ("noheuleprocess", "Don't try to do global/local substitution per Heule")
    ;

    po::options_description gatesOptions("Gates' options");
    gatesOptions.add_options()
    ("nogates", "Don't find gates. Disable ALL below")
    ("nogorshort", "Don't shorten clauses with OR gates")
    ("nogandrem", "Don't remove clauses with AND gates ")
    ("nogeqlit", "Don't find equivalent literals using gates")
    ("noer", "Don't find gates to add to do ER")
    ("printgatedot", "Print gate structure regularly to file 'gatesX.dot'")
    ;

    #ifdef USE_GAUSS
    po::options_description gaussOptions("Gaussian options");
    gaussOptions.add_options()
    ("gaussuntil", po::value<uint32_t>()->default_value(gaussconfig.decision_until), "Cutoff decision depth for gauss. Default IS ZERO, so Gauss is OFF!!!!!!")
    ("nosepmatrix", "Don't separate independent matrixes")
    ("noordercol", "Don't order variables in the columns. Disables iterative matrix reduction")
    ("noiterreduce", "Don't reduce iteratively the matrix")
    ("maxmatrixrows", po::value<uint32_t>()->default_value(gaussconfig.maxMatrixRows), "Max. num of rows for gaussian matrix")
    ("minmatrixrows", po::value<uint32_t>()->default_value(gaussconfig.minMatrixRows), "Min. num of rows for gaussian matrix")
    ("savematrix", po::value<uint32_t>()->default_value(gaussconfig.only_nth_gauss_save), "Leave matrix trail every Nth decision level")
    ("maxnummatrixes", po::value<uint32_t>()->default_value(gaussconfig.maxNumMatrixes), "Max. num matrixes")
    ;
    #endif //USE_GAUSS

    po::options_description conflOptions("Conflict options");
    conflOptions.add_options()
    ("norecminim", "Don't do MiniSat-type conflict-clause minim.")
    ("nolfminim", "Don't do strong minimisation at conflict gen.")
    ("noalwaysfmin", "Don't always strong-minimise clause")
    ("printimpldot", "Print implication graph DOT files")
    ;

    po::options_description xorOptions("XOR-related options");
    xorOptions.add_options()
    ("nolxorfind", "Don't find long(2+) XORs during subsumption")
    ("nobinxorfind", "Don't find equivalent literals through SCC")
    ("noextendedscc", "Don't do SCC using non-exist. bins")
    ("novarreplace", "Don't perform variable replacement")
    ("nomix", "Don't mix XORs and OrGates for new truths")
    ;

    po::options_description miscOptions("Misc options");
    miscOptions.add_options()
    ("nopresimp", "Don't perform simplification at the beginning")
    //("noparts", "Don't find&solve subproblems with subsolvers")
    ("noclausevivif", "Don't do regular clause vivification")
    ("nosortwatched", "Don't sort watches according to size")
    ("nocalcreach", "Don't calculate literal reachability")
    ("nocache", "No implication cache. Less memory used, disables LOTS")
    ;

    po::positional_options_description p;
    p.add("input", -1);

    po::variables_map vm;
    po::options_description cmdline_options;
    cmdline_options
    .add(generalOptions)
    .add(conflOptions)
    .add(iterativeOptions)
    .add(failedLitOptions)
    .add(sateliteOptions)
    .add(xorOptions)
    //.add(xorSateliteOptions)
    .add(gatesOptions)
    #ifdef USE_GAUSS
    .add(gaussOptions)
    #endif
    .add(miscOptions)
    ;

    po::store(po::command_line_parser(argc, argv).options(cmdline_options).positional(p).run(), vm);
    po::notify(vm);

    if (vm.count("help")) {
        std::cout
        << "USAGE: " << argv[0] << " [options] <input-files>" << std::endl
        << " where input is "
        #ifdef DISABLE_ZLIB
        << "plain"
        #else
        << "plain or gzipped"
        #endif // DISABLE_ZLIB
        << " DIMACS." << std::endl;

        std::cout << cmdline_options << std::endl;
        exit(0);
    }

    if (vm.count("polarity")) {
        std::string mode = vm["polarity"].as<std::string>();

        if (mode == "true") conf.polarity_mode = polarity_true;
        else if (mode == "false") conf.polarity_mode = polarity_false;
        else if (mode == "rnd") conf.polarity_mode = polarity_rnd;
        else if (mode == "auto") conf.polarity_mode = polarity_auto;
        else throw WrongParam(mode, "unknown polarity-mode");
    }

    if (vm.count("rnd-freq")) {
        conf.random_var_freq = vm["rnd-freq"].as<double>();
        if (conf.random_var_freq < 0 || conf.random_var_freq > 1) {
            std::cout << "ERROR! illegal rnRSE ERROR!d-freq constant " << conf.random_var_freq << std::endl;
            exit(-1);
        }
    }

    //Conflict
    if (vm.count("noalwaysfmin")) {
        conf.doAlwaysFMinim = false;
    }
    if (vm.count("nolfminim")) {
        conf.doMinimLearntMore = false;
    }
    if (vm.count("norecminim")) {
        conf.expensive_ccmin = false;
    }
    if (vm.count("printimpldot")) {
        conf.doPrintConflDot = true;
    }

    if (vm.count("verbosity")) {
        conf.verbosity = vm["verbosity"].as<int>();
    }

    if (vm.count("randomize")) {
        conf.origSeed = vm["randomize"].as<uint32_t>();
    }

    if (vm.count("dumplearnts")) {
        conf.learntsFilename = vm["dumplearnts"].as<std::string>();
        conf.needToDumpLearnts = true;
    }

    if (vm.count("dumporig")) {
        conf.origFilename = vm["dumporig"].as<std::string>();
        conf.needToDumpOrig = true;
    }

    if (vm.count("maxdump")) {
        if (!conf.needToDumpLearnts) throw WrongParam("maxdumplearnts", "--dumplearnts=<filename> must be first activated before issuing -maxdumplearnts=<size>");

        conf.maxDumpLearntsSize = vm["maxdumplearnts"].as<uint32_t>();
    }

    if (vm.count("maxsolutions")) {
        max_nr_of_solutions = vm["maxsolutions"].as<uint32_t>();
    }

    if (vm.count("nobanfoundsol")) {
        doBanFoundSolution = false;
    }

    if (vm.count("pavgbranch")) {
        conf.doPrintAvgBranch = true;
    }

    if (vm.count("greedyunbound")) {
        conf.greedyUnbound = true;
    }

    //XOR finding
    if (vm.count("nobinxorfind")) {
        conf.doFindEqLits = false;
    }

    if (vm.count("noextendedscc")) {
        conf.doExtendedSCC = false;
    }

    if (vm.count("nolxorfind")) {
        conf.doFindXors = false;
    }

    if (vm.count("nosimplify")) {
        conf.doSchedSimp = false;
    }

    if (vm.count("debuglib")) {
        debugLib = true;
    }

    if (vm.count("debugnewvar")) {
        debugNewVar = true;
    }

    if (vm.count("novarreplace")) {
        conf.doReplace = false;
    }

    if (vm.count("nofailedlit")) {
        conf.doFailedLit = false;
    }

    #ifdef USE_GAUSS
    if (vm.count("gaussuntil")) {
        gaussconfig.decision_until = vm["gaussuntil"].as<uint32_t>();
    }

    if (vm.count("nodisablegauss")) {
        gaussconfig.dontDisable = true;
    }

    if (vm.count("maxnummatrixes")) {
        gaussconfig.maxNumMatrixes = vm["maxnummatrixes"].as<uint32_t>();
    }

    if (vm.count("nosepmatrix")) {
        gaussconfig.doSeparateMatrixFind = false;
    }

    if (vm.count("noiterreduce")) {
        gaussconfig.iterativeReduce = false;
    }

    if (vm.count("noiterreduce")) {
        gaussconfig.iterativeReduce = false;
    }

    if (vm.count("noordercol")) {
        gaussconfig.orderCols = false;
    }

    if (vm.count("maxmatrixrows")) {
        gaussconfig.maxMatrixRows = vm["maxmatrixrows"].as<uint32_t>();
    }


    if (vm.count("minmatrixrows")) {
        gaussconfig.minMatrixRows = vm["minmatrixrows"].as<uint32_t>();
    }


    if (vm.count("savematrix")) {
        gaussconfig.only_nth_gauss_save = vm["savematrix"].as<uint32_t>();
    }
    #endif //USE_GAUSS

    if (vm.count("nosatelite")) {
        conf.doSatELite = false;
    }

    if (vm.count("nohyperbinres")) {
        conf.doHyperBinRes = false;
    }

    if (vm.count("novarelim")) {
        conf.doVarElim = false;
    }

    if (vm.count("nosubsume1")) {
        conf.doSubsume1 = false;
    }

    if (vm.count("restart")) {
        std::string type = vm["restart"].as<std::string>();
        if (type == "auto")
            conf.fixRestartType = auto_restart;
        else if (type == "static")
            conf.fixRestartType = static_restart;
        else if (type == "dynamic")
            conf.fixRestartType = dynamic_restart;
        else throw WrongParam("restart", "unknown restart type");
    }

    if (vm.count("nosolprint")) {
        printResult = false;
    }

    if (vm.count("nohyperbinres")) {
        conf.doHyperBinRes= false;
    }

    if (vm.count("noremovebins")) {
        conf.doRemUselessBins = false;
    }

    if (vm.count("noclausevivif")) {
        conf.doClausVivif = false;
    }

    if (vm.count("nosortwatched")) {
        conf.doSortWatched = false;
    }

    if (vm.count("printImplDot")) {
        conf.doPrintConflDot = true;
    }

    if (vm.count("nocalcreach")) {
        conf.doCalcReach = false;
    }

    if (vm.count("nocache")) {
        conf.doCache = false;
    }

    if (vm.count("noblocked")) {
        conf.doBlockedClause = false;
    }

    if (vm.count("nogates")) {
        conf.doGateFind = false;
    }

    if (vm.count("noer")) {
        conf.doER = false;
    }

    if (vm.count("nogorshort")) {
        conf.doShortenWithOrGates = false;
    }

    if (vm.count("nogandrem")) {
        conf.doRemClWithAndGates = false;
    }

    if (vm.count("nogeqlit")) {
        conf.doFindEqLitsWithGates = false;

    }

    if (vm.count("printgatedot")) {
        conf.doPrintGateDot = true;

    }

    if (vm.count("nopresimp")) {
        conf.doPerformPreSimp = false;

    }

    if (vm.count("threads")) {
        numThreads = vm["threads"].as<uint32_t>();
        if (numThreads < 1) throw WrongParam("threads", "Num threads must be at least 1");
    }

    if (vm.count("input")) {
        filesToRead = vm["input"].as<std::vector<std::string> >();
        fileNamePresent = true;
    } else {
        fileNamePresent = false;
    }

    if (vm.count("output")) {
        outputFile = vm["output"].as<std::string>();
    }

    if (conf.verbosity >= 1) {
        std::cout << "c Outputting solution to console" << std::endl;
    }

    if (!debugLib) conf.libraryUsage = false;
}

void Main::printVersionInfo(const uint32_t verbosity)
{
    if (verbosity >= 1) {
        std::cout << "c This is CryptoMiniSat " << VERSION << std::endl;
        #ifdef __GNUC__
        std::cout << "c compiled with gcc version " << __VERSION__ << std::endl;
        #else
        std::cout << "c compiled with non-gcc compiler" << std::endl;
        #endif
    }
}

int Main::solve()
{
    control = new ThreadControl(conf);
    solverToInterrupt = control;

    printVersionInfo(conf.verbosity);
    parseInAllFiles();

    unsigned long current_nr_of_solutions = 0;
    lbool ret = l_True;
    while(current_nr_of_solutions < max_nr_of_solutions && ret == l_True) {
        ret = control->solve(numThreads);
        current_nr_of_solutions++;

        if (ret == l_True && current_nr_of_solutions < max_nr_of_solutions) {
            if (conf.verbosity >= 1) std::cout << "c Prepare for next run..." << std::endl;
            printResultFunc(ret);

            if (doBanFoundSolution) {
                vector<Lit> lits;
                for (Var var = 0; var != control->nVars(); var++) {
                    if (control->model[var] != l_Undef) {
                        lits.push_back( Lit(var, (control->model[var] == l_True)? true : false) );
                    }
                }
                control->addClause(lits);
            }
        }
    }

    /*
    if (conf.needToDumpLearnts) {
        control->dumpSortedLearnts(conf.learntsFilename, conf.maxDumpLearntsSize);
        std::cout << "c Sorted learnt clauses dumped to file '" << conf.learntsFilename << "'" << std::endl;
    }
    if (conf.needToDumpOrig) {
        if (ret == l_False && conf.origFilename == "stdout") {
            std::cout << "p cnf 0 1" << std::endl;
            std::cout << "0";
        } else if (ret == l_True && conf.origFilename == "stdout") {
            std::cout << "p cnf " << control->model.size() << " " << control->model.size() << std::endl;
            for (uint32_t i = 0; i < control->model.size(); i++) {
                std::cout << (control->model[i] == l_True ? "" : "-") << i+1 << " 0" << std::endl;
            }
        } else {
            control->dumpOrigClauses(conf.origFilename);
            if (conf.verbosity >= 1)
                std::cout << "c Simplified original clauses dumped to file '"
                << conf.origFilename << "'" << std::endl;
        }
    }*/

    if (ret == l_Undef && conf.verbosity >= 1) {
        std::cout << "c Not finished running -- signal caught or maximum restart reached" << std::endl;
    }
    if (conf.verbosity >= 1) control->printStats();

    printResultFunc(ret);

    return correctReturnValue(ret);
}

int Main::correctReturnValue(const lbool ret) const
{
    int retval = -1;
    if      (ret == l_True)  retval = 10;
    else if (ret == l_False) retval = 20;
    else if (ret == l_Undef) retval = 15;
    else {
        std::cerr << "Something is very wrong, output is neither l_Undef, nor l_False, nor l_True" << std::endl;
        exit(-1);
    }

    #ifdef NDEBUG
    // (faster than "return", which will invoke the destructor for 'Solver')
    exit(retval);
    #endif
    return retval;
}

int main(int argc, char** argv)
{
    Main main(argc, argv);

    try {
        main.parseCommandLine();
    } catch (boost::exception_detail::clone_impl<boost::exception_detail::error_info_injector<boost::program_options::unknown_option> >& c) {
        std::cout << "ERROR! Some option you gave was wrong. Please give '--help' to get help" << std::endl;
        std::cout << "Unparsed option: '" << c.get_option_name() << "'" << std::endl;
        exit(-1);
    } catch (boost::bad_any_cast &e) {
        std::cerr << "ERROR! You probably gave a wrong argument type (Bad cast). " << e.what() << std::endl;
        exit(-1);
    } catch (boost::exception_detail::clone_impl<boost::exception_detail::error_info_injector<boost::program_options::invalid_option_value> > what) {
        std::cerr << "ERROR! " << what.what() << std::endl;
        exit(-1);
    } catch (boost::exception_detail::clone_impl<boost::exception_detail::error_info_injector<boost::program_options::multiple_occurrences> >& c) {
        std::cerr << "ERROR! Multiple occurrences of option: " << c.get_option_name() << std::endl;
        exit(-1);
    } catch (WrongParam& w) {
        std::cerr << "ERROR! Option parameter '" << w.getParam() << "' is wrong" << std::endl;
        std::cerr << "Specific error message: " << w.getMsg() << std::endl;
        exit(-1);
    }

    signal(SIGINT, SIGINT_handler);
    //signal(SIGHUP,SIGINT_handler);

    return main.solve();
}
