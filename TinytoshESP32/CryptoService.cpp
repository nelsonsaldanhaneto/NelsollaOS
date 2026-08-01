#include "CryptoService.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>

bool CryptoService::fetchPrice(int id, CryptoData &data) {
    HTTPClient http;
    String url = String(CRYPTO_API_URL) + "?id=" + String(id);

    Serial.printf("CryptoService: Requesting Crypto Data from CoinLore: %d\n", id); 
    Serial.printf("CryptoService: URL: %s\n", url.c_str()); 
    http.setReuse(false); 
    http.begin(url);
    http.setConnectTimeout(5000); 
    http.setTimeout(5000);
    int httpCode = http.GET();

    if (httpCode == 200) {
        String payload = http.getString();
        // CoinLore's ticker payload is ~360 bytes of JSON, but ArduinoJson also
        // needs room for the object tree and its copies of every key and string
        // value, which overran 512 and failed every fetch with NoMemory.
        StaticJsonDocument<1024> doc;
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
            JsonObject obj = doc[0];
            data.name = obj["name"].as<String>();
            data.symbol = obj["symbol"].as<String>();
            data.price_usd = obj["price_usd"].as<float>();
            data.percent_change_24h = obj["percent_change_24h"].as<float>();
            data.updated = true;
            
            Serial.printf("CryptoService: Success! %s (%s): $%.2f (24h Change: %.2f%%)\n", 
                          data.name.c_str(), data.symbol.c_str(), 
                          data.price_usd, data.percent_change_24h);
                          
            http.end();
            return true;
        } else {
            Serial.printf("CryptoService: JSON parsing failed: %s\n", error.c_str());
        }
    } else {
        Serial.printf("CryptoService: API failed, HTTP Code: %d\n", httpCode);
    }
    
    http.end();
    return false;
}