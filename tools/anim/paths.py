"""Where the extracted inputs live (all under the git-ignored extracted/)."""
import os

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
# The installer (installer/install.py) points this at a temporary folder.
EXTRACTED = os.environ.get("JC2MECH_EXTRACTED") or os.path.join(REPO, "extracted")
JC2_SKELETON = os.path.join(EXTRACTED, "5b8827ed", "biped.bsk")
JC2_ANIMS = os.path.join(EXTRACTED, "310dc808")
SKYRIM_SKELETON = os.path.join(EXTRACTED, "skyrim", "meshes", "actors", "character", "character assets", "skeleton.hkx")
