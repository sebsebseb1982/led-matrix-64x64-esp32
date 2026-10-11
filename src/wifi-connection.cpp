#include <Arduino.h>
#include <WiFi.h>
#include "wifi-connection.h"
#include "secrets.h"

namespace WifiConnection {

void setup() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // latence plus faible pour enchainer les snapshots
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connexion WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP : ");
  Serial.println(WiFi.localIP());
}

bool ensureConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.println("WiFi perdu, reconnexion...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  return WiFi.status() == WL_CONNECTED;
}

}
