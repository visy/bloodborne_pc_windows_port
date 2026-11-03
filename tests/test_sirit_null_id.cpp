// SPDX-License-Identifier: GPL-2.0-or-later
// A null Id operand in a SPIR-V instruction (a translator bug in one shader) is a recoverable
// error, not an abort of the process: the shader cache rebuild skips that shader.
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <sirit/sirit.h>

int main() {
    Sirit::Module module{0x00010600};
    const Sirit::Id u32_type = module.TypeInt(32, false);
    const Sirit::Id ptr_type = module.TypePointer(spv::StorageClass::Function, u32_type);
    const Sirit::Id index = module.Constant(u32_type, 0u);
    const std::vector<Sirit::Id> indices{index};
    bool thrown = false;
    try {
        module.OpAccessChain(ptr_type, Sirit::Id{}, indices);
    } catch (const std::runtime_error& e) {
        thrown = std::string(e.what()).find("null Id") != std::string::npos;
    }
    if (!thrown) {
        std::puts("FAIL: a null Id operand did not throw");
        return 1;
    }
    std::puts("Sirit: a null Id operand throws a recoverable error");
    return 0;
}
