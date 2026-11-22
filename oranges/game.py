"""The one game build oranges supports. Lifted code, addresses and patches are specific to it."""

from .nup import Nup

TITLE_ID = 0x01006F8002326000
# main's NSO module id. "3.0.3" appears once as a string in main; that it is the game version is not confirmed.
MAIN_BUILD_ID = "ff1d1c05670db6021c85b624a710b963"
SHORT_BUILD = MAIN_BUILD_ID[:8]
SDK_VERSION = "20.5.4"
EXEFS_MODULES = ("rtld", "main", "subsdk0", "sdk")   # load order


def check(nup: Nup) -> tuple[bool, str]:
    """Whether the package holds the supported build, and why not."""
    title = next((t for t in nup.titles if t.id == TITLE_ID), None)
    if title is None:
        return False, "no title %016X in the package" % TITLE_ID
    build = nup.build_id(title)
    if build is None:
        from . import nso
        build = nso.load(nup.read(nup.find(title, "exefs/main"))).build_id
    if build != MAIN_BUILD_ID:
        return False, "build %s is not the supported build %s" % (build, SHORT_BUILD)
    return True, "supported build %s" % SHORT_BUILD
