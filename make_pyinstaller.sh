#!/bin/bash
NUMCPUS=`grep -c '^processor' /proc/cpuinfo` # Get max number of CPU threads
NUMCPUS=$(echo "($NUMCPUS*0.75+0.5)/1" | bc) # Set CPU threads to 3/4th avail. threads, rounding to nearest whole number
printf "\033[33;1mMake sure you've installed OpenCL and OpenBLAS by using \"sudo apt install libclblast-dev libopenblas-dev\"\n\n\n\n\n\n\033[0m\n"
sleep 4
# install dependencies
pip install pyinstaller customtkinter && make clean && \
# Ensure all backends are built then build executable file
make LLAMA_HIPBLAS=1 LLAMA_VULKAN=1 LLAMA_OPENBLAS=1 -j$NUMCPUS && \
pyinstaller --noconfirm --onefile --clean --console --collect-all customtkinter --collect-all libclblast-dev --collect-all clinfo --icon ".\niko.ico" \
--add-data "./kcpp_adapters:./kcpp_adapters" \
--add-data "./koboldcpp.py:." \
--add-data "./json_to_gbnf.py:." \
--add-data "./launch.cmd:." \
--add-data "./requirements_minimal.txt:." \
--add-data "./LICENSE.md:."  \
--add-data "./MIT_LICENSE_GGML_SDCPP_LLAMACPP_ONLY.md:." \
--add-data "./klite.embd:." \
--add-data "./kcpp_docs.embd:." \
--add-data "./kcpp_sdui.embd:." \
--add-data "./taesd.embd:." \
--add-data "./taesd_xl.embd:." \
--add-data "./taesd_f.embd:." \
--add-data "./taesd_3.embd:." \
--add-data "./koboldcpp_default.so:." \
--add-data "./koboldcpp_hipblas.so:." \
--add-data "/opt/rocm/lib/libhipblas.so:." \
--add-data "/opt/rocm/lib/librocblas.so:." \
--add-data "./rwkv_vocab.embd:." \
--add-data "./rwkv_world_vocab.embd:." \
--add-data "/opt/rocm/lib/rocblas:." \
--version-file "./version.txt" \
"./koboldcpp.py" -n "koboldcpp_rocm"
