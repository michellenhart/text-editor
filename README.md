# text-editor

# Compilar:
    mpicc editor_colaborativo.c -o editor_colaborativo -fopenmp `pkg-config --cflags --libs gtk+-3.0`
# Executar:
    mpirun -np 2 ./editor_colaborativo
