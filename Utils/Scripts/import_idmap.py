import struct
import os
import ida_kernwin
import ida_nalt
import ida_name
import ida_idaapi

def import_functions():
    file_path = ida_kernwin.ask_file(0, "*.idmap", "Load the .idmap file")
    if not file_path:
        print("IDAExecFunctionsImporter: Cancelled by user.")
        return

    print(f"IDAExecFunctionsImporter: Processing {file_path}...")
    
    image_base = ida_nalt.get_imagebase()
    print(f"IDAExecFunctionsImporter: Image base is 0x{image_base:X}")
    print("IDAExecFunctionsImporter: Applying names...")

    count = 0
    try:
        with open(file_path, "rb") as f:
            f.seek(0, os.SEEK_END)
            file_size = f.tell()
            f.seek(0, os.SEEK_SET)

            while f.tell() < file_size:
                offset_data = f.read(4)
                if len(offset_data) < 4: break
                offset = struct.unpack("<I", offset_data)[0]

                len_data = f.read(2)
                if len(len_data) < 2: break
                name_len = struct.unpack("<H", len_data)[0]

                name_bytes = f.read(name_len)
                if len(name_bytes) < name_len: break
                
                try:
                    name = name_bytes.decode('utf-8')
                except UnicodeDecodeError:
                    name = name_bytes.decode('latin-1', errors='replace')

                ea = image_base + offset
                ida_name.set_name(ea, name)
                
                count += 1

    except Exception as e:
        print(f"IDAExecFunctionsImporter: Error reading file: {e}")
        return

    print(f"IDAExecFunctionsImporter: Done. Imported {count} names.")
    ida_kernwin.request_refresh(ida_kernwin.IWID_DISASMS)

if __name__ == "__main__":
    import_functions()
