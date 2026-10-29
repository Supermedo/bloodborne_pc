# tools/Fsr4SpirvCrossFixes.pm: corrections to spirv-cross GLSL of the FSR 4 v07 passes,
# shared by the rewrite scripts (fsr4_post_lds.pl, fsr4_pass11_guard.pl).
# A change here rebuilds fsr4_shaders/opt (tools/fsr4_optimize.sh).
package Fsr4SpirvCrossFixes;
use strict;
use warnings;

# spirv-cross translates OpBitcast uint -> v4char (signed int8) as unpack8(uint), which returns
# an unsigned u8vec4. The passes only ever unpack signed int8 tensors (no OpBitcast to v4uchar),
# so every unpack8 goes through a signed helper. Returns the source with the helper declared
# before main.
sub signed_unpack {
    my ($src) = @_;
    my $n = ($src =~ s/\bunpack8\(/bbUnpackS8(/g);
    die "expected signed int8 unpacks\n" unless $n;
    my $helper = <<'GLSL';
// bbport: signed int8 unpack (spirv-cross emits the unsigned unpack8(uint)).
i8vec4 bbUnpackS8(uint v)
{
    return unpack8(int(v));
}

GLSL
    $src =~ s/^(void main\(\)\n)/$helper$1/m or die "no main\n";
    return $src;
}

# spirv-cross declares the passes' weight tables as global const arrays. glslang gives a const
# array a function-local copy (an "indexable" variable initialized with the whole array) at every
# dynamic index into it: 16 copies of a 512-entry table in pass 11, 32 in post. AMD's Windows
# driver keeps the copies in scratch memory (RX 6700 XT, 1080p: pass 11 19.6 ms instead of
# 0.2 ms, post 26.2 instead of 0.4). Declared without const, each table is one private array.
# Returns the source with const dropped from the global array declarations.
sub single_tables {
    my ($src) = @_;
    my $n = ($src =~ s/^const (\w+ _\d+\[\d+\]) = /$1 = /mg);
    die "expected const tables\n" unless $n;
    return $src;
}

1;
