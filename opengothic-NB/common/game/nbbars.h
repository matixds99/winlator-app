#pragma once

#include <array>
#include <string>

/// NB fix 41: the bars of the Union plugin of New Balance (Bar_SetShow, Bar_SetValue, ...). The scripts fill them every
/// frame (PerFrame -> B_UpdateStaminaBar), the window draws them. Index 1 is the stamina (STAMINABAR_INDEX), 2..5 the
/// life of the target (OpenGothic draws its own).
namespace NbBars {
  struct Bar {
    bool        show     = false;
    std::string tex;
    int         value    = 0;
    int         maxRange = 0;
    };

  constexpr int Count   = 8;
  constexpr int Stamina = 1;

  inline std::array<Bar,Count>& all() {
    static std::array<Bar,Count> bars;
    return bars;
    }

  inline Bar* at(int i) {
    return (0<=i && i<Count) ? &all()[size_t(i)] : nullptr;
    }

  inline void reset() {
    all() = {};
    }
  }
