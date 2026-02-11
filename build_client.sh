#!/bin/bash
# Build polymetis Python client for Python 3.11 + PyTorch 2.9
# Only builds torch_isolation (FK/IK C++ libs) + pb2 stubs.
# Does NOT build C++ server (already running in Docker).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
POLYMETIS_DIR="$SCRIPT_DIR/polymetis"

echo "=== Step 1: Install C++ build deps via conda ==="
echo "Running: mamba install pinocchio eigen spdlog cmake"
mamba install -y -c conda-forge pinocchio eigen spdlog cmake

echo ""
echo "=== Step 2: Install Python grpc_tools (for pb2 generation) ==="
pip install grpcio-tools

echo ""
echo "=== Step 3: Build torch_isolation (libtorchscript_pinocchio.so) ==="
cd "$POLYMETIS_DIR/torch_isolation"
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=TRUE
make -j$(nproc)
echo "Built libraries:"
find . -name "*.so" -type f

echo ""
echo "=== Step 4: Generate pb2 Python stubs from .proto ==="
cd "$POLYMETIS_DIR"

# Generate polymetis_pb2.py into existing package dir
python -m grpc_tools.protoc \
    -I protos \
    --python_out=python/polymetis_pb2 \
    protos/polymetis.proto

# Generate polymetis_pb2_grpc.py
mkdir -p python/polymetis_pb2_grpc
python -m grpc_tools.protoc \
    -I protos \
    --grpc_python_out=python/polymetis_pb2_grpc \
    protos/polymetis.proto

# Ensure __init__.py re-exports correctly
echo "from .polymetis_pb2_grpc import *" > python/polymetis_pb2_grpc/__init__.py

echo "Generated pb2 files:"
ls -la python/polymetis_pb2/polymetis_pb2.py
ls -la python/polymetis_pb2_grpc/polymetis_pb2_grpc.py

echo ""
echo "=== Step 5: Install polymetis Python package ==="
cd "$POLYMETIS_DIR"
pip install -e . --no-deps

echo ""
echo "=== Step 6: Verify ==="
python -c "
from polymetis import RobotInterface, GripperInterface
print('  polymetis import OK')
print('  Connecting to localhost:50051 ...')
try:
    r = RobotInterface(ip_address='localhost', port=50051)
    print('  Robot joints:', r.get_joint_positions().tolist())
except Exception as e:
    print('  (server not running, but import works)')
"

echo ""
echo "=== Done ==="
