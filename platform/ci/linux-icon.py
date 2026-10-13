#!/usr/bin/env python3
"""Export the existing game icon for Linux desktop packages."""
import sys
from pathlib import Path

from PIL import Image

repository = Path(__file__).resolve().parents[2]
destination = Path(sys.argv[1])
destination.parent.mkdir(parents=True, exist_ok=True)
with Image.open(repository / "source/blood/rsrc/game_icon.ico") as icon:
    icon.convert("RGBA").resize((48, 48), Image.Resampling.LANCZOS).save(destination)
