@echo off
rem Opens the PicoGB loader (Chrome or Edge) to add, play and delete games on the Sprig.
cd /d "%~dp0loader"
python serve.py
