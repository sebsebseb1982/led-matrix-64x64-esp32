#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Camera {
  // Alloue le tampon JPEG (a appeler une fois le WiFi connecte)
  bool setup();
  // Telecharge un snapshot JPEG ; renvoie sa taille (0 en cas d'erreur).
  // Le pointeur reste valide jusqu'au prochain appel.
  size_t fetchSnapshot(uint8_t** data);
}
