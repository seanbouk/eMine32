@echo off
rem Serve the web build at http://localhost:8032/eduke32.html  (add ?stats for timing in the JS console)
cd /d "%~dp0web\out"
start "" http://localhost:8032/eduke32.html
python -m http.server 8032
