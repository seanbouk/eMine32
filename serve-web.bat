@echo off
rem Serve the web build at http://localhost:8032/eduke32.html with caching off (add ?stats for timing)
start "" http://localhost:8032/eduke32.html
python "%~dp0tools\serve.py" 8032
