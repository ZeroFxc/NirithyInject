#include "Patcher.h"
#include "Il2cpp/il2cpp-class.h"
#include "KittyMemory/KittyMemory.h"

using namespace asmjit;

#ifdef __DEBUG__
    #define ASSERR(error)                                                                                              \
        do                                                                                                             \
        {                                                                                                              \
            if (error != kErrorOk)                                                                                     \
                LOGE("ASSERR: %s => %s", #error, asmjit::DebugUtils::errorAsString(error));                            \
        } while (0)
#else
    #define ASSERR(error) (error)
#endif

Patcher::Patcher(MethodInfo *method) : target{method->methodPointer}
{
    using namespace asmjit;
#ifdef __aarch64__
    code.init(Environment(Arch::kAArch64));
#else
    code.init(Environment(Arch::kARM));
#endif
    code.attach(&assembler);
}
Error Patcher::ret()
{
#ifdef __aarch64__
    ASSERR(assembler.ret(asmjit::a64::x30));
#else
    ASSERR(assembler.bx(asmjit::a32::lr));
#endif
    return asmjit::kErrorOk;
}

Error Patcher::movInt16(int16_t value)
{
#ifdef __aarch64__
    ASSERR(assembler.movz(a64::x0, value));
#else
    ASSERR(assembler.movw(a32::r0, value));
#endif
    return asmjit::kErrorOk;
}

Error Patcher::movUInt16(uint16_t value)
{
#ifdef __aarch64__
    ASSERR(assembler.movz(a64::x0, value));
#else
    ASSERR(assembler.movw(a32::r0, value));
#endif
    return asmjit::kErrorOk;
}

asmjit::Error Patcher::movInt32(int32_t value)
{
    uint16_t lowerBit = static_cast<uint16_t>(value);
    uint16_t higherBit = static_cast<uint16_t>(value >> 16);
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::w0, lowerBit));
    ASSERR(assembler.movk(a64::w0, higherBit, a64::lsl(16)));
#else
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}
asmjit::Error Patcher::movUInt32(uint32_t value)
{
    uint16_t lowerBit = static_cast<uint16_t>(value);
    uint16_t higherBit = static_cast<uint16_t>(value >> 16);
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::w0, lowerBit));
    ASSERR(assembler.movk(a64::w0, higherBit, a64::lsl(16)));
#else
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}

asmjit::Error Patcher::movInt64(int64_t value)
{
    uint16_t lowerBit = static_cast<uint16_t>(value);
    uint16_t higherBit = static_cast<uint16_t>(value >> 16);
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::w0, lowerBit));
    ASSERR(assembler.movk(a64::w0, higherBit, a64::lsl(16)));
#else
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}
asmjit::Error Patcher::movUInt64(uint64_t value)
{
    uint16_t lowerBit = static_cast<uint16_t>(value);
    uint16_t higherBit = static_cast<uint16_t>(value >> 16);
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::w0, lowerBit));
    ASSERR(assembler.movk(a64::w0, higherBit, a64::lsl(16)));
#else
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}

asmjit::Error Patcher::movFloat(float value)
{
    union FloatBits
    {
        float f;
        uint32_t i;
    };
    FloatBits fb{value};

    uint16_t lowerBit = static_cast<uint16_t>(fb.i);
    uint16_t higherBit = static_cast<uint16_t>(fb.i >> 16);
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::w0, lowerBit));
    ASSERR(assembler.movk(a64::w0, higherBit, a64::lsl(16)));
    ASSERR(assembler.fmov(a64::s0, a64::w0));
#else
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}

asmjit::Error Patcher::movBool(bool value)
{
    int b = value ? 1 : 0;
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::x0, b));
#else
    ASSERR(assembler.mov(a32::r0, b));
#endif
    return asmjit::kErrorOk;
}

asmjit::Error Patcher::movPtr(void *value)
{
#ifdef __aarch64__
    auto val = imm(value);
    ASSERR(assembler.mov(a64::x0, val));
#else
    uint16_t lowerBit = static_cast<uint16_t>((uintptr_t)value);
    uint16_t higherBit = static_cast<uint16_t>((uintptr_t)value >> 16);
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}

asmjit::Error Patcher::redirect(void *destination)
{
    if (!destination)
        return asmjit::kErrorInvalidArgument;

    const uintptr_t dst = reinterpret_cast<uintptr_t>(destination);
    const uintptr_t src = reinterpret_cast<uintptr_t>(target);
    asmjit::Error err = asmjit::kErrorOk;

#ifdef __aarch64__
    // AArch64 的 b 指令要求目标地址 4 字节对齐
    if (dst & 0x3)
    {
        LOGE("redirect target is not 4-byte aligned: %p", destination);
        return asmjit::kErrorInvalidArgument;
    }

    // b 指令编码：0x14000000 | imm26，imm26 = (target - pc) / 4，为 26 位有符号数，
    // 相对偏移范围 ±128MB。asmjit 的 a64 分支指令以 Imm 作为绝对地址，
    // 但本 CodeHolder 未设置基址，无法自动计算相对位移，故此处手工编码。
    const int64_t delta = static_cast<int64_t>(dst) - static_cast<int64_t>(src);
    if (delta >= -(1ll << 27) && delta <= (1ll << 27) - 4)
    {
        const uint32_t insn = 0x14000000u | (static_cast<uint32_t>(delta >> 2) & 0x03FFFFFFu);
        err = assembler.embed(&insn, sizeof(insn));
        ASSERR(err);
    }
    else
    {
        // 超出 ±128MB：ldr x16, #8（字面量装载）; br x16; 后跟 8 字节绝对地址
        const uint32_t trampoline[2] = {0x58000050u, 0xD61F0200u};
        err = assembler.embed(trampoline, sizeof(trampoline));
        ASSERR(err);
        const uint64_t addr = dst;
        err = assembler.embed(&addr, sizeof(addr));
        ASSERR(err);
    }
#else
    // ARM(A32) 模式同样要求 4 字节对齐；asmjit 的 a32 分支指令不支持 Imm 操作数，
    // 因此统一手工编码 b 指令。
    if (dst & 0x3)
    {
        LOGE("redirect target is not 4-byte aligned: %p", destination);
        return asmjit::kErrorInvalidArgument;
    }

    // ARM 模式下 PC = 当前指令地址 + 8，imm24 = (target - (pc + 8)) / 4，
    // 为 24 位有符号数，相对偏移范围 ±32MB。
    const int64_t delta = static_cast<int64_t>(dst) - static_cast<int64_t>(src) - 8;
    if (delta >= -(1ll << 25) && delta <= (1ll << 25) - 4)
    {
        const uint32_t insn = 0xEA000000u | (static_cast<uint32_t>(delta >> 2) & 0x00FFFFFFu);
        err = assembler.embed(&insn, sizeof(insn));
        ASSERR(err);
    }
    else
    {
        // 超出 ±32MB：movw r12, lo; movt r12, hi; bx r12
        err = assembler.movw(a32::r12, static_cast<uint16_t>(dst));
        ASSERR(err);
        err = assembler.movt(a32::r12, static_cast<uint16_t>(dst >> 16));
        ASSERR(err);
        err = assembler.bx(a32::r12);
        ASSERR(err);
    }
#endif

    return err;
}

std::vector<uint8_t> Patcher::patch()
{
    std::vector<char> bytes;
    for (auto s : code.sections())
    {
        for (auto c : s->buffer())
        {
            LOGD("%02X", c);
            bytes.push_back(c);
        }
    }

    auto rawBytes = bytes.data();
    auto protect = KittyMemory::ProtectAddr(target, bytes.size(), PROT_READ | PROT_WRITE | PROT_EXEC);
    LOGINT(protect);

    std::vector<uint8_t> originalBytes((uint8_t *)target, (uint8_t *)target + bytes.size());
    for (auto b : originalBytes)
    {
        LOGD("%02X", b);
    }

    memcpy(target, (void *)rawBytes, bytes.size());
    return originalBytes;
}
