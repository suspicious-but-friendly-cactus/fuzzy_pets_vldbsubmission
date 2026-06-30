
#include <droidCrypto/psi/PhasedPSIServer.h>
#include <droidCrypto/ChannelWrapper.h>
#include <droidCrypto/psi/OPRFAESPSIServer.h>
#include <droidCrypto/psi/OPRFLowMCPSIServer.h>
#include <droidCrypto/psi/ECNRPSIServer.h>
#include <thread>
#include <iostream>

//namespace oprf{

int main(int argc, char** argv) {

    std::string arg_port_str("-port");
    std::string arg_prf_str("-prf");

    int port = 50051;
    std::string prf_type = "ECNR";
    bool loop = false;

    for (int i=1; i < argc;) {
        std::string arg(argv[i]);
        if(arg == arg_port_str && i + 1 < argc) {
            port = std::stoi(argv[i+1]);
            i += 2;
        } else if(arg == arg_prf_str && i + 1 < argc) {
            prf_type = argv[i+1];
            if ((prf_type != "ECNR") && (prf_type != "GCAES") && (prf_type != "GCLOWMC")) {
                std::cout << "The correct argument syntax is -port <PORT> -prf <ECNR|GCAES|GCLOWMC> [-loop]"
                    << std::endl;
                return 0;
            }
            i += 2;
        } else if(arg == "-loop" || arg == "--loop") {
            loop = true;
            i += 1;
        } else {
            std::cout << "The correct argument syntax is -port <PORT> -prf <ECNR|GCAES|GCLOWMC> [-loop]"
                << std::endl;
            return 0;
        }
    }
    do {
        if (prf_type == "ECNR") {      
            std::cout << "Start ECNR-OPRF Server on port " << port <<"\n";
            droidCrypto::CSocketChannel chan(nullptr, port, true);
            droidCrypto::ECNRPSIServer server(chan, 1);
            server.doOPRF();
            std::cout << "Done ECNR-OPRF\n";
        } else if (prf_type == "GCAES") {
            std::cout << "Start GCAES-OPRF Server on port " << port <<"\n";  
            droidCrypto::CSocketChannel chan(nullptr, port, true);
            droidCrypto::OPRFAESPSIServer server(chan, 1);
            server.doOPRF();
            std::cout << "Done GCAES-OPRF\n";
        } else if (prf_type == "GCLOWMC") {
            std::cout << "Start GCLowMC-OPRF Server on port " << port <<"\n"; 
            droidCrypto::CSocketChannel chan(nullptr, port, true);
            droidCrypto::OPRFLowMCPSIServer server(chan, 1);
            server.doOPRF();
            std::cout << "Done GCLowMC-OPRF\n";
        }
    } while (loop);
    return 1;
}
