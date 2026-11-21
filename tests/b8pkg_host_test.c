#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../include/b8pkg.h"

static const char valid_manifest[] =
    "[Package]\n"
    "name = \"paint-book\" ; name\n"
    "version = 1.2.3\n"
    "entry = app.ELF\n"
    "[Dependencies]\n"
    "libs = ui.b8s, net-core.B8S # required libraries\n"
    "[Associations]\n"
    ".TXT = TextEditor\n"
    ".b8 = Book8.Document\n"
    "[UI]\n"
    "start_menu = yes\n";

static bool invalid(const char *text)
{
    struct b8pkg_manifest manifest;
    memset(&manifest, 0x5a, sizeof(manifest));
    struct b8pkg_manifest before = manifest;
    int32_t result = b8pkg_parse(text, strlen(text), &manifest);
    assert(result == -BOOK8_EINVAL);
    assert(memcmp(&manifest, &before, sizeof(manifest)) == 0);
    return true;
}

int main(void)
{
    struct b8pkg_manifest manifest;
    assert(b8pkg_parse(valid_manifest, strlen(valid_manifest), &manifest) == 0);
    assert(strcmp(manifest.name, "paint-book") == 0);
    assert(strcmp(manifest.version, "1.2.3") == 0);
    assert(strcmp(manifest.entry, "app.ELF") == 0);
    assert(manifest.min_uid == 1000u);
    assert(manifest.privilege == B8PKG_PRIVILEGE_USER);
    assert(manifest.start_menu);
    assert(manifest.library_count == 2u);
    assert(strcmp(manifest.libraries[1], "net-core.B8S") == 0);
    assert(manifest.association_count == 2u);
    assert(strcmp(manifest.associations[0].extension, ".txt") == 0);

    invalid("[Package]\nversion=1.0.0\nentry=app.elf\n");
    invalid("[Package]\nname=test\nname=other\nversion=1.0.0\nentry=app.elf\n");
    invalid("[Package]\nname=test\nversion=1.0.0\nentry=../escape.elf\n");
    invalid("[Package]\nname=test\nversion=1.0.0\nentry=app.elf\n"
        "[Associations]\n.TXT=TextEditor\n.txt=OtherEditor\n");
    invalid("[Package]\nname=test\nversion=1.0.0\nentry=app.elf\n"
        "privilege=root\nmin_uid=1\n");
    invalid("[Package]\nname=test\nversion=01.0.0\nentry=app.elf\n");
    invalid("[Package]\nname=test\nversion=1.0.0\nentry=A:/app.elf\n");
    puts("PASS: b8pkg parsing and schema validation");
    return 0;
}
