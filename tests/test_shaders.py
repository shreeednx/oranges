"""The back end's checked-in SPIR-V headers (host/vk/shaders/*_spv.h) match their sources: oranges.shaders --check.
Without glslang it compares the source hash each header names; with it, the SPIR-V words too."""
from oranges import shaders


def test_headers_match_sources():
    said = []
    assert shaders.run(check=True, say=said.append) == 0, "\n".join(said)
    assert any("match their sources" in s for s in said)


def test_every_shader_has_a_header_and_its_include_folder():
    comps = sorted(shaders.SHADERS.glob("*.comp"))
    assert {c.stem for c in comps} >= {"fsr1_easu", "fsr1_rcas"}
    for comp in comps:
        assert shaders.header_path(comp).exists()
        assert all(d.is_dir() for d in shaders.includes(comp))


def test_a_changed_source_is_found(tmp_path, monkeypatch):
    for name in ("fsr1_rcas.comp", "fsr1_rcas_comp_spv.h"):
        (tmp_path / name).write_bytes((shaders.SHADERS / name).read_bytes())
    monkeypatch.setattr(shaders, "SHADERS", tmp_path)
    monkeypatch.setattr(shaders, "glslang", lambda: None)   # (the hash alone finds it)
    comp = tmp_path / "fsr1_rcas.comp"
    comp.write_text(comp.read_text() + "\n// changed\n")
    said = []
    assert shaders.run(check=True, say=said.append) == 1
    assert "another source" in said[0]
