HEATWAVE WATCH - WebAssembly build (for Vercel)

index.html     the page (design + the JS that loads the wasm)
heatwave.wasm  the C program compiled to WebAssembly (runs in the browser)
heatwave.c     the C source code (same file also builds the native server)
build_wasm.sh  command used to produce heatwave.wasm  (run with: sh build_wasm.sh)

Deploy: upload this folder to a GitHub repo, import it on vercel.com
(Framework Preset: Other, no build command, no output directory).
Local test: python3 -m http.server 8000   then open http://localhost:8000
(Opening index.html by double-click will NOT work: browsers block wasm from file://)
