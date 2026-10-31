#pragma once
#include <string>

#include "app.h"

struct Probe {
    std::string override;
    bool checkService = false;
    std::pair<Level, std::wstring> client, net, login, service;
    bool hasService = false;
    std::string boxName, boxDomain, loginUser, cookie;
};

// Runs the LazyCat probe on a worker thread and posts WM_APP_PROBE with a Probe*.
void lzcProbeAsync(bool checkService);
