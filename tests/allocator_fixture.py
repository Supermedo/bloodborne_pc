"""Independent D015 fixture: only the documented 393-byte allocator function.

Source: TASK-001 character-fault-static.json, function 0x26addc0. Its bytes are
data for signatures, never executed as original count instructions on this host.
This fixture has no dependency on game installation or CPU implementation.
"""
from cpu_harness import CF, ZF, SF, OF, ARITHMETIC_FLAGS

SOURCE_SHA256 = 'd65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9'
FUNCTION_SHA256 = '16729bb23b2f814ef311ef0a3f3aac2d91f16110a206885eae694f2b17f38133'
FUNCTION_START, FUNCTION_END = 0x26addc0, 0x26adf49
FUNCTION_BYTES = bytes.fromhex(
    '554889e54157415653504889d34989f74989fe31c04881fb000001000f875c010000'
    '8d43fff30fbdc0b92000000029c1b8ffffffffd3e041238690000000f30fbcc883f910'
    '7262498b3ebe00000100e83cbfffff4889c24885d27526498b3e4c89fee839caffff'
    '498b3ebe00000100e81cbfffff4889c231c04885d20f84fc000000488b4208'
    '81fb000001000f85b9000000498b8e8800000048894a2849899688000000e9d5000000'
    '498b74ce08488b1689d04803460801d348891e81fb000001007533488b5628'
    '498954ce08f3480fbdd2c1ea06d3e2f7d241219690000000498b8e88000000'
    '48894e284989b688000000e987000000ba0000010029daf30fbdfaba1f000000'
    '29fa39d17471488b7e28498b5cd60849897cce08498974d60848895e28f3480fbdf7'
    'c1ee06d3e6c4c248f2b690000000bf0100000088d1d3e709f74189be90000000'
    'eb33b90000010029d9f30fbdf1b91f00000029f189de488932498b74ce08'
    '48897228498954ce08ba01000000d3e2410996900000004883c4085b415e415f5dc3')

# (address, mnemonic, operands, source64, destination64, tail destination64,
#  immediate, exact original, exact tail, SUB context bytes)
SPANS = (
    (0x26adde5, 'lzcnt', 'eax, eax', 'rax', 'rax', 'rcx', 32,
     'f30fbdc0b920000000', 'b920000000', '29c1'),
    (0x26addfe, 'tzcnt', 'ecx, eax', 'rax', 'rcx', None, 16,
     'f30fbcc883f910', '83f910', ''),
    (0x26adebe, 'lzcnt', 'edi, edx', 'rdx', 'rdi', 'rdx', 31,
     'f30fbdfaba1f000000', 'ba1f000000', '29fa'),
    (0x26adf12, 'lzcnt', 'esi, ecx', 'rcx', 'rsi', 'rcx', 31,
     'f30fbdf1b91f000000', 'b91f000000', '29f1'),
)


def subtract32(left, right):
    """Independent SUB/CMP flags, including parity and half-carry."""
    left, right = left & 0xffffffff, right & 0xffffffff
    result = (left - right) & 0xffffffff
    flags = (CF if left < right else 0) | (ZF if result == 0 else 0)
    flags |= SF if result & 0x80000000 else 0
    flags |= OF if (left ^ right) & (left ^ result) & 0x80000000 else 0
    flags |= 16 if (left ^ right ^ result) & 16 else 0
    flags |= 4 if (result & 255).bit_count() % 2 == 0 else 0
    return result, flags


def expected_state(span, registers, guest_rsp, *, include_context=False):
    """Return every GPR, defined flags and mask after the complete sequence."""
    _, mnemonic, _, source, destination, tail_destination, immediate, *_ = span
    expected = dict(registers, rsp=guest_rsp)
    value = registers[source] & 0xffffffff
    if mnemonic == 'lzcnt':
        count = 32 - value.bit_length()
    else:
        count = (value & -value).bit_length() - 1 if value else 32
    expected[destination] = count
    flags, mask = (CF if value == 0 else 0) | (ZF if count == 0 else 0), CF | ZF
    if tail_destination is None:
        _, flags = subtract32(count, immediate)
        mask = ARITHMETIC_FLAGS
    else:
        expected[tail_destination] = immediate
        if include_context:
            expected[tail_destination], flags = subtract32(immediate, count)
            mask = ARITHMETIC_FLAGS
    return expected, flags, mask
