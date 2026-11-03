// SPDX-License-Identifier: GPL-2.0-or-later
// SubresourceExtent::Contains (Mrsuss60 6b2cc0e0 / shadPS4 ca36c7e3): both dimensions must fit,
// where the defaulted lexicographic compare orders by levels first.
#include <cassert>
#include <cstdio>

#include "video_core/texture_cache/types.h"

using VideoCore::SubresourceExtent;

int main() {
    const SubresourceExtent mips_few_layers{5, 1};   // 5 mips, 1 layer
    const SubresourceExtent one_mip_many_layers{1, 6}; // a cube: 1 mip, 6 layers
    const SubresourceExtent both{5, 6};

    // The lexicographic order calls the first "bigger" than the second although it holds only one
    // of the six layers.
    assert(mips_few_layers > one_mip_many_layers);
    assert(!mips_few_layers.Contains(one_mip_many_layers));
    assert(!one_mip_many_layers.Contains(mips_few_layers));
    assert(both.Contains(mips_few_layers));
    assert(both.Contains(one_mip_many_layers));
    assert(both.Contains(both));
    assert(!mips_few_layers.Contains(both));
    assert(SubresourceExtent{}.Contains(SubresourceExtent{}));
    std::puts("subresource extent: ok");
    return 0;
}
