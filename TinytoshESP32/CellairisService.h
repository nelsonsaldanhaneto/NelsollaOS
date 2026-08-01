#ifndef CELLAIRIS_SERVICE_H
#define CELLAIRIS_SERVICE_H

#include <Arduino.h>
#include "structs.h"

class CellairisService {
public:
    bool fetchSummary(const Config& config, CellairisData& data);

private:
    static constexpr const char* API_URL = "https://gestao-cellairis.vercel.app/api/tinytosh";
};

#endif
