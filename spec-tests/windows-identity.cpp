// Uses the production DPAPI adapter under a fixture-only LOCALAPPDATA directory.
#include "../lzc-mesh-client/windows/src/app.h"
#include "../lzc-mesh-client/windows/src/identity.h"
#include <iostream>
App g;
void setStatus(int,Level level,const std::wstring &) {if(level==L_ERROR)std::cerr<<"Device credential rejected\n";}
std::wstring widen(const std::string &value){return std::wstring(value.begin(),value.end());}
int main(int argc,char **argv) {
    if(argc<2)return 2;
    if(!SetEnvironmentVariableA("LOCALAPPDATA",argv[1]))return 3;
    g.deviceId="fixture-windows";
    if(!identity::init())return 1;
    std::cout<<identity::fingerprint()<<'\n'<<identity::publicKey()<<'\n';
    if(argc>2)std::cout<<identity::proof("fixture-account",argv[2])<<'\n';
    identity::shutdown();return 0;
}
