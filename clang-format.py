import os

c_extensions = (".c", ".h")
directories = ["kernel", "user", "notxv6"]

for dd in directories:
    for root, dirs, files in os.walk(dd):
        for file in files:
            if file.endswith(c_extensions):
                os.system("clang-format -i -style=file " + root + "/" + file)
