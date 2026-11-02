#pragma once
#include <string>
namespace identity {
bool init();
std::string publicKey();
std::string fingerprint();
std::string proof(const std::string &user,const std::string &nonce);
void shutdown();
}
