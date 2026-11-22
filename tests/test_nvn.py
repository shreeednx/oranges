"""The NVN layer's proc table (host/nvn/nvn_procs.inc) lists exactly the procs host/nvn_native.c implements."""
import re
from pathlib import Path

HOST = Path(__file__).resolve().parent.parent / "host"


def test_proc_table_matches_implementations():
    impl = re.findall(r"^NV\((\w+)\)", (HOST / "nvn_native.c").read_text(), re.M)
    table = re.findall(r"^\s*E\((\w+)\),", (HOST / "nvn" / "nvn_procs.inc").read_text(), re.M)
    assert len(impl) == len(set(impl)), "an NV() is defined twice"
    assert sorted(impl) == sorted(table), (
        "nvn_procs.inc is out of date: missing %s, extra %s" % (sorted(set(impl) - set(table)),
                                                                 sorted(set(table) - set(impl))))
    assert all(n.startswith("nvn") for n in impl)
