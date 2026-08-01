#include "CellairisService.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

bool CellairisService::fetchSummary(const Config& config, CellairisData& data) {
    if (config.cellairis_token.length() == 0) {
        Serial.println("CellairisService: token vazio, pulando fetch.");
        return false;
    }

    Serial.println("CellairisService: Buscando resumo mensal do gestao-cellairis...");

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setReuse(false);
    http.begin(client, API_URL);
    http.addHeader("Authorization", "Bearer " + config.cellairis_token);
    http.setConnectTimeout(10000);
    http.setTimeout(10000);

    int httpCode = http.GET();
    if (httpCode != 200) {
        Serial.printf("CellairisService: API falhou, HTTP %d\n", httpCode);
        http.end();
        return false;
    }

    String payload = http.getString();
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, payload);
    http.end();

    if (error) {
        Serial.printf("CellairisService: JSON invalido: %s\n", error.c_str());
        return false;
    }

    data.mes      = doc["mes"].as<String>();
    data.faturado = doc["faturado"].as<float>();
    data.projecao = doc["projecao"].as<float>();
    data.meta     = doc["meta"].as<float>();
    data.pct      = doc["pct"].as<float>();
    data.proj_pct = doc["proj_pct"].as<float>();
    data.updated  = true;

    Serial.printf("CellairisService: OK! %s faturado R$%.0f, projecao R$%.0f (meta R$%.0f, %.1f%%)\n",
                  data.mes.c_str(), data.faturado, data.projecao, data.meta, data.pct);
    return true;
}
