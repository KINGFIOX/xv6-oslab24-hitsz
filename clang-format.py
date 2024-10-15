import os

c_extensions = (".c")

for root, dirs, files in os.walk("kernel"):
    for file in files:
        if file.endswith(c_extensions):
            os.system("clang-format -i -style=file " + root + "/" + file)

for root, dirs, files in os.walk("user"):
    for file in files:
        if file.endswith(c_extensions):
            os.system("clang-format -i -style=file " + root + "/" + file)