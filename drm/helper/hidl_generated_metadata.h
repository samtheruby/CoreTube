// SPDX-License-Identifier: BSD-3-Clause
#include <array>

// Newer generated headers add enum metadata; retain Android 9's runtime ABI
// headers.
namespace android::hardware::details {
template <typename T>
inline constexpr std::array<T, 0> hidl_enum_values = {};
}
