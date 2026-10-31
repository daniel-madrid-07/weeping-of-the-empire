# Compile Builder.exe into the repository root (make_setup.py --builder); a release's Setup exe: make_setup.py --tag
py -3 (Join-Path $PSScriptRoot 'make_setup.py') --builder
