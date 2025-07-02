# text-editor
   Edtitor de textos colaborativo.
# Instalações necessárias:
    sudo apt-get update
    sudo apt install build-essential gdb
    sudo apt install openmpi-bin openmpi-common libopenmpi-dev
    sudo apt-get install libgtk-3-dev
# Compilar:
    mpicc editor-colaborativo.c -o editor-colaborativo -fopenmp `pkg-config --cflags --libs gtk+-3.0`
# Executar:
    mpirun -np 2 ./editor-colaborativo
