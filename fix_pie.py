import re
import glob

DIAG_PIE = """
        <pie_menu
            label="Diagnostics >"
            name="Diagnostics">
            <pie_slice
                label="Dump Anim UUIDs"
                name="Dump Active Animation UUIDs">
                <pie_slice.on_click
                    function="Avatar.DumpActiveMotions" />
            </pie_slice>
        </pie_menu>
"""

def fix_file(filepath, target_name):
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()

    # Strip existing Diagnostics blocks
    content = re.sub(r'(?s)\s*<pie_menu[^>]*name="Diagnostics"\s*>.*?</pie_menu>', '', content)

    # Inject Diagnostics block right after the target opening tag
    pattern = r'(<pie_menu[^>]*name="' + target_name + r'"[^>]*>)'
    
    if not re.search(pattern, content):
        print(f"WARNING: Target {target_name} not found in {filepath}")
        return

    content = re.sub(pattern, r'\1' + DIAG_PIE, content, count=1)

    with open(filepath, 'w', encoding='utf-8') as f:
        f.write(content)

for slang in glob.glob("indra/newview/skins/default/xui/*/menu_pie_avatar_self.xml"):
    fix_file(slang, r"Appearance &gt;")

for slang in glob.glob("indra/newview/skins/default/xui/*/menu_pie_avatar_other.xml"):
    fix_file(slang, r"Avatar Pie More 2")

for slang in glob.glob("indra/newview/skins/default/xui/*/menu_pie_attachment_self.xml"):
    fix_file(slang, r"Attachment Pie More")

for slang in glob.glob("indra/newview/skins/default/xui/*/menu_pie_attachment_other.xml"):
    fix_file(slang, r"Avatar Pie More 2")

print("Done fixing pie menus")
