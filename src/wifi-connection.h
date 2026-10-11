#pragma once

namespace WifiConnection {
  void setup();
  // Reconnecte si le lien est perdu ; renvoie true si connecte
  bool ensureConnected();
}
