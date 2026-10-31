"""Check the tracked source boundary before publication; standard library only."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
for p in root.rglob('*'):
    rel=p.relative_to(root)
    if any(x in {'.git','build','managed_components','__pycache__'} for x in rel.parts) or not p.is_file():continue
    assert not any(x in {'private','bridge','libde265','de265'} for x in rel.parts),rel
    assert p.suffix not in {'.pcm','.wav','.mp4','.raw','.bin'},rel
    if p.suffix in {'.c','.h','.js','.html','.cmake'} or p.name=='CMakeLists.txt':
        text=p.read_text()
        assert not re.search(r'motion_(?:init|video|enabled|notify)|board_io|board_play_alert|/api/(?:motion|notify|alert)',text),rel
        assert not re.search(r'/(?:Users|home)/[^/\s]+/',text),rel
        if rel.parts[0] != 'third_party':
            assert not re.search(r'\b(?:10\.\d+|192\.168|172\.(?:1[6-9]|2\d|3[01]))\.\d+\.\d+\b',text),rel
print('Public source boundary: PASS')
