Dear ImGui v1.92.9b, from https://github.com/ocornut/imgui (tag v1.92.9b, archive sha256
21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99), unmodified.
MIT license: LICENSE.txt.

Copied from Longshot's third_party/imgui (https://git.magicbean.dev/MagicBeanDev/Longshot, commit 0e32bc1), which keeps
the same files of that release: the core (imgui*.cpp, imgui*.h, imstb_*.h, imconfig.h) and the Vulkan renderer backend
(backends/imgui_impl_vulkan.*). Left out: the demo, the examples, the docs, the other backends and misc/.
Settings are given as compiler defines by oranges/build.py, not by editing imconfig.h.

Used by host/vk/vk_menu.cpp: the settings menu, drawn over the window's image with ImGui's draw lists. ImGui's own
widgets and input are not used (the menu's model and input are host/hos_menu.c's).
