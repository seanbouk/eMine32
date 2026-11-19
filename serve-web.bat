@echo off
rem Serve the web build at http://localhost:8032/ with caching off (add ?stats for timing)
start "" http://localhost:8032/
python "%~dp0tools\serve.py" 8032
