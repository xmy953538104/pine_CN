// SPDX-License-Identifier: MPL-2.0
#include <catalog/catalog.h>
#include <cassert>

using namespace skyline::catalog;
int main(int argc, char **argv) {
    assert(argc == 2);
    Options options; options.profile = ContentInfoProfile::Modern40;
    auto source = OpenFile(argv[1]);
    auto group = ParseCnmt(source, 0, "independent-literal", options);
    assert(group.parsing == "parsed");
    assert(group.platform == 0 && group.metaAttributes == 4);
    assert(group.storageId == 5 && group.installType == 1 && group.committedRaw == 1);
    assert(group.contentSupport == "unsupported");
    assert(group.requiredDownloadSystemVersion == 0x04030201);
    assert(group.contentTableOffset == 0x38 && group.metaTableOffset == 0x70);
    assert(group.contents.size() == 1 && group.metaReferences.size() == 1);
    assert(group.contents[0].contentId == "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");
    assert(group.metaReferences[0].titleId == 0x8877665544332211ULL);
}
