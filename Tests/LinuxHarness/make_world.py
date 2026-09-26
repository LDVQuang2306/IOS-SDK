#!/usr/bin/env python3
"""
Builds a replica of a real game's reflection data for the Linux harness from a Dumper-7 dump folder
(GObjects-Dump-WithProperties.txt + CppSDK/): every object with its class and outer, every package, class and struct
(super, size, alignment), property (kind, offset, size, flags and the types it references), function (flags and
parameters) and enum (values).

The harness then rebuilds that object graph in the Delta Force memory layout (DF_HARNESS_WORLD=<world.json>), so the
dumper can be run on Linux against the dependency graph of the real game:

    python3 Tests/LinuxHarness/make_world.py ~/Downloads/1.203.37117_65-DeltaForce build/world.json
    DF_HARNESS_WORLD=build/world.json ./Tests/LinuxHarness/run.sh
"""
import json
import os
import re
import sys
from collections import defaultdict

PROPERTY_FLAGS = {
    'Edit': 0x1, 'ConstParm': 0x2, 'BlueprintVisible': 0x4, 'ExportObject': 0x8, 'BlueprintReadOnly': 0x10, 'Net': 0x20,
    'EditFixedSize': 0x40, 'Parm': 0x80, 'OutParm': 0x100, 'ZeroConstructor': 0x200, 'ReturnParm': 0x400,
    'DisableEditOnTemplate': 0x800, 'Transient': 0x2000, 'Config': 0x4000, 'DisableEditOnInstance': 0x10000,
    'EditConst': 0x20000, 'GlobalConfig': 0x40000, 'InstancedReference': 0x80000, 'DuplicateTransient': 0x200000,
    'SubobjectReference': 0x400000, 'SaveGame': 0x1000000, 'NoClear': 0x2000000, 'ReferenceParm': 0x8000000,
    'BlueprintAssignable': 0x10000000, 'Deprecated': 0x20000000, 'IsPlainOldData': 0x40000000, 'RepSkip': 0x80000000,
    'RepNotify': 0x100000000, 'Interp': 0x200000000, 'NonTransactional': 0x400000000, 'EditorOnly': 0x800000000,
    'NoDestructor': 0x1000000000, 'AutoWeak': 0x4000000000, 'ContainsInstancedReference': 0x8000000000,
    'AssetRegistrySearchable': 0x10000000000, 'SimpleDisplay': 0x20000000000, 'AdvancedDisplay': 0x40000000000,
    'Protected': 0x80000000000, 'BlueprintCallable': 0x100000000000, 'BlueprintAuthorityOnly': 0x200000000000,
    'TextExportTransient': 0x400000000000, 'NonPIEDuplicateTransient': 0x800000000000, 'ExposeOnSpawn': 0x1000000000000,
    'PersistentInstance': 0x2000000000000, 'UObjectWrapper': 0x4000000000000, 'HasGetValueTypeHash': 0x8000000000000,
    'NativeAccessSpecifierPublic': 0x10000000000000, 'NativeAccessSpecifierProtected': 0x20000000000000,
    'NativeAccessSpecifierPrivate': 0x40000000000000, 'SkipSerialization': 0x80000000000000,
}

FUNCTION_FLAGS = {
    'Final': 0x1, 'RequiredAPI': 0x2, 'BlueprintAuthorityOnly': 0x4, 'BlueprintCosmetic': 0x8, 'Net': 0x40,
    'NetReliable': 0x80, 'NetRequest': 0x100, 'Exec': 0x200, 'Native': 0x400, 'Event': 0x800, 'NetResponse': 0x1000,
    'Static': 0x2000, 'NetMulticast': 0x4000, 'UbergraphFunction': 0x8000, 'MulticastDelegate': 0x10000,
    'Public': 0x20000, 'Private': 0x40000, 'Protected': 0x80000, 'Delegate': 0x100000, 'NetServer': 0x200000,
    'HasOutParams': 0x400000, 'HasDefaults': 0x800000, 'NetClient': 0x1000000, 'DLLImport': 0x2000000,
    'BlueprintCallable': 0x4000000, 'BlueprintEvent': 0x8000000, 'BlueprintPure': 0x10000000, 'EditorOnly': 0x20000000,
    'Const': 0x40000000, 'NetValidate': 0x80000000,
}

CPF_Parm, CPF_OutParm, CPF_ReturnParm, CPF_ConstParm, CPF_ReferenceParm = 0x80, 0x100, 0x400, 0x2, 0x8000000
CPF_UObjectWrapper = 0x4000000000000

NUMERIC = {
    'uint8': ('ByteProperty', 1), 'int8': ('Int8Property', 1), 'uint16': ('UInt16Property', 2), 'int16': ('Int16Property', 2),
    'uint32': ('UInt32Property', 4), 'int32': ('IntProperty', 4), 'uint64': ('UInt64Property', 8), 'int64': ('Int64Property', 8),
    'float': ('FloatProperty', 4), 'double': ('DoubleProperty', 8),
}

OBJECT_RE = re.compile(r'^\[([0-9A-F]{8})\] \{0x[0-9a-fA-F]+\} (\S+) (.*)$')
PROPERTY_RE = re.compile(r'^\[([0-9A-F]{8})\] \{0x[0-9a-fA-F]+\}\t(\S+) (.*)$')

HEADER_RE = re.compile(r'^// (\S+) (.+)\n// 0x([0-9A-F]+) \(0x([0-9A-F]+) - 0x([0-9A-F]+)\)\n(#pragma pack\(push, 0x1\)\n)?'
                       r'(?:template<[^\n]*>\n)?(?:struct|class|union) (?:alignas\(0x([0-9A-F]+)\) )?([\w:]+)[^\n]*?(?: : public ([\w:]+))?\n\{\n', re.M)
ENUM_RE = re.compile(r'^// Enum ([^\n]+)\n// NumValues: 0x[0-9A-F]+\nenum class ([\w:]+) : (\w+)\n\{\n(.*?)\n\};', re.M | re.S)
ENUM_VALUE_RE = re.compile(r'^\t(\w+)\s*= (-?\d+),', re.M)
FUNCTION_RE = re.compile(r'^// (Function|DelegateFunction|SparseDelegateFunction) (.+)\n// \((.*)\)$', re.M)
ALIGN_RE = re.compile(r'^static_assert\(alignof\(([\w:]+)\) == 0x([0-9A-F]+),', re.M)
MEMBER_RE = re.compile(r'^\t(.*?);\s+// 0x([0-9A-F]+)\(0x([0-9A-F]+)\)\((.*)\)\s*$')
DECL_RE = re.compile(r'^(.*?)\s+([A-Za-z_]\w*)(?:\[(0x[0-9A-Fa-f]+)\])?(?:\s*:\s*(\d+))?$')


def log(*args):
    print('[make_world]', *args, file=sys.stderr)


def split_top_level(text, sep=','):
    parts, depth, cur = [], 0, ''
    for ch in text:
        if ch in '<(':
            depth += 1
        elif ch in '>)':
            depth -= 1
        if ch == sep and depth == 0:
            parts.append(cur)
            cur = ''
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    return [p.strip() for p in parts]


def parse_flags(text):
    """'(BitIndex: 0x03, PropSize: 0x0001 (Edit, Net))' or 'Edit, Net' -> (flags, bit index)"""
    bit = None
    m = re.match(r'BitIndex: 0x([0-9A-F]+), PropSize: 0x[0-9A-F]+ \((.*)\)$', text)
    if m:
        bit = int(m.group(1), 16)
        text = m.group(2)
    flags = 0
    for name in text.split(','):
        flags |= PROPERTY_FLAGS.get(name.strip(), 0)
    return flags, bit


class World:
    def __init__(self, dump):
        self.dump = dump
        self.objects = []            # [class name, path, name]
        self.props = {}              # object index -> [(offset, kind, name)]
        self.by_full_name = {}       # 'Class Engine.Actor' -> index
        self.by_path = {}
        self.cpp_to_index = {}       # 'AActor' / 'GameplayAbilities::FServerAbilityRPCBatch' -> index
        self.struct_info = {}        # index -> dict(size, align, super_cpp, members)
        self.enum_info = {}          # index -> (underlying, [(name, value)])
        self.enum_underlying = {}    # enum cpp name -> underlying type
        self.function_flags = {}     # index -> flags
        self.stats = defaultdict(int)
        self.examples = defaultdict(list)

    # ---------------------------------------------------------------- GObjects dump

    def read_objects(self):
        current = None
        with open(os.path.join(self.dump, 'GObjects-Dump-WithProperties.txt'), encoding='utf-8', errors='replace') as f:
            for line in f:
                line = line.rstrip('\n')
                m = PROPERTY_RE.match(line)
                if m:
                    if current is not None:
                        self.props.setdefault(current, []).append((int(m.group(1), 16), m.group(2), m.group(3)))
                    continue
                m = OBJECT_RE.match(line)
                if not m:
                    continue
                index = int(m.group(1), 16)
                while len(self.objects) < index:
                    self.objects.append(None)
                self.objects.append([m.group(2), m.group(3)])
                current = index

        for index, obj in enumerate(self.objects):
            if obj is None:
                continue
            self.by_full_name.setdefault(f'{obj[0]} {obj[1]}', index)
            self.by_path.setdefault(obj[1], index)
        log(f'{len(self.objects)} object slots, {sum(len(p) for p in self.props.values())} properties')

    def resolve_outers(self):
        self.outer = [-1] * len(self.objects)
        self.names = [''] * len(self.objects)
        for index, obj in enumerate(self.objects):
            if obj is None:
                continue
            path = obj[1]
            outer, name = -1, path
            pos = len(path)
            while True:
                pos = path.rfind('.', 0, pos)
                if pos <= 0:
                    break
                candidate = self.by_path.get(path[:pos])
                if candidate is not None and candidate != index:
                    outer, name = candidate, path[pos + 1:]
                    break
            self.outer[index] = outer
            self.names[index] = name

    def resolve_classes(self):
        class_kinds = {'Class', 'BlueprintGeneratedClass', 'WidgetBlueprintGeneratedClass', 'AnimBlueprintGeneratedClass', 'LinkerPlaceholderClass'}
        classes_by_name = {}
        for index, obj in enumerate(self.objects):
            if obj is not None and (obj[0] in class_kinds or obj[0].endswith('GeneratedClass')):
                classes_by_name.setdefault(self.names[index], index)
        self.class_index = [-1] * len(self.objects)
        missing = set()
        for index, obj in enumerate(self.objects):
            if obj is None:
                continue
            c = classes_by_name.get(obj[0])
            if c is None:
                missing.add(obj[0])
            else:
                self.class_index[index] = c
        self.classes_by_name = classes_by_name
        if missing:
            log('classes of objects not found:', sorted(missing)[:20])

    # ---------------------------------------------------------------- CppSDK

    def read_sdk(self):
        sdk = os.path.join(self.dump, 'CppSDK', 'SDK')
        files = sorted(os.listdir(sdk))
        for file in files:
            path = os.path.join(sdk, file)
            with open(path, encoding='utf-8-sig', errors='replace') as f:
                text = f.read()
            if file.endswith('_functions.cpp'):
                for m in FUNCTION_RE.finditer(text):
                    index = self.by_full_name.get(f'{m.group(1)} {m.group(2)}', self.by_path.get(m.group(2)))
                    if index is None:
                        continue
                    flags = 0
                    for name in m.group(3).split(','):
                        flags |= FUNCTION_FLAGS.get(name.strip(), 0)
                    self.function_flags[index] = flags
                continue
            if not file.endswith('.hpp'):
                continue
            for m in ENUM_RE.finditer(text):
                index = self.by_full_name.get('Enum ' + m.group(1), self.by_path.get(m.group(1)))
                cpp, underlying = m.group(2), m.group(3)
                values = []
                for v in ENUM_VALUE_RE.finditer(m.group(4)):
                    value = int(v.group(2))
                    if value >= 1 << 63:
                        value -= 1 << 64
                    values.append((v.group(1), value))
                self.enum_underlying[cpp] = underlying
                if index is not None:
                    self.cpp_to_index[cpp] = index
                    self.enum_info[index] = (underlying, values)
            alignments = {a.group(1): int(a.group(2), 16) for a in ALIGN_RE.finditer(text)}
            for m in HEADER_RE.finditer(text):
                kind, full = m.group(1), m.group(2)
                index = self.by_full_name.get(f'{kind} {full}', self.by_path.get(full))
                if index is None:
                    self.stats['sdk struct without object'] += 1
                    continue
                body_start = m.end()
                body_end = text.find('\n};', body_start)
                members = []
                for line in text[body_start:body_end].split('\n'):
                    mm = MEMBER_RE.match(line)
                    if not mm:
                        continue
                    comment = mm.group(4)
                    if 'NOT AUTO-GENERATED PROPERTY' in comment or '[ Dumper-7 ]' in comment:
                        continue
                    d = DECL_RE.match(mm.group(1).strip())
                    if not d:
                        self.stats['unparsed member'] += 1
                        continue
                    flags, bit = parse_flags(comment)
                    members.append(dict(type=d.group(1).strip(), name=d.group(2), dim=int(d.group(3), 16) if d.group(3) else 1,
                                        bitfield=d.group(4) is not None, offset=int(mm.group(2), 16), size=int(mm.group(3), 16),
                                        flags=flags, bit=bit))
                cpp = m.group(8)
                # The alignment the generator computed (static_assert(alignof(...))), UStruct::MinAlignment has the same value in the game
                align = alignments.get(cpp, int(m.group(7), 16) if m.group(7) else 0)
                info = dict(kind=kind, total=int(m.group(4), 16), align=align,
                            packed=m.group(6) is not None, super_cpp=m.group(9), members=members, cpp=cpp)
                self.struct_info[index] = info
                if kind != 'Function':
                    self.cpp_to_index[cpp] = index
        log(f'{len(self.struct_info)} structs/classes/functions and {len(self.enum_info)} enums from the CppSDK')

        # Interfaces are declared without their base class ('class IAssetRegistry final'), the Dumpspace files have the real super
        self.inherit = {}
        for file in ('ClassesInfo.json', 'StructsInfo.json'):
            path = os.path.join(self.dump, 'Dumpspace', file)
            if not os.path.exists(path):
                continue
            with open(path) as f:
                for entry in json.load(f)['data']:
                    for cpp, members in entry.items():
                        for member in members:
                            if '__InheritInfo' in member and member['__InheritInfo']:
                                self.inherit[cpp] = member['__InheritInfo'][0]

    # ---------------------------------------------------------------- types

    def type_index(self, cpp):
        cpp = cpp.strip()
        index = self.cpp_to_index.get(cpp)
        if index is None and '::' in cpp:
            index = self.cpp_to_index.get(cpp.split('::')[-1])
        return index

    def descriptor(self, kind, text, size=0):
        """Property descriptor for a member of the given kind with the given C++ type text (from the CppSDK)"""
        text = text.strip()
        if text.startswith('const '):
            text = text[6:].strip()
        d = dict(k=kind, s=size)

        def inner_of(prefix):
            return text[len(prefix):-1].strip()

        def class_of(t):
            t = t.strip()
            if t.startswith('class '):
                t = t[6:]
            return self.type_index(t.rstrip('*').strip())

        if kind == 'StructProperty':
            d['struct'] = self.type_index(text[7:] if text.startswith('struct ') else text)
            if d['struct'] is None:
                return None
        elif kind in ('ObjectProperty', 'ObjectPtrProperty'):
            d['cls'] = class_of(text)
            if d['cls'] is None:
                return None
        elif kind == 'ClassProperty':
            d['cls'] = self.type_index('UClass')
            if text.startswith('TSubclassOf<'):
                d['meta'] = class_of(inner_of('TSubclassOf<'))
                d['f'] = CPF_UObjectWrapper
            else:
                d['meta'] = self.type_index('UObject')
        elif kind in ('WeakObjectProperty', 'LazyObjectProperty', 'SoftObjectProperty'):
            prefix = {'WeakObjectProperty': 'TWeakObjectPtr<', 'LazyObjectProperty': 'TLazyObjectPtr<', 'SoftObjectProperty': 'TSoftObjectPtr<'}[kind]
            if not text.startswith(prefix):
                return None
            d['cls'] = class_of(inner_of(prefix))
            if d['cls'] is None:
                return None
        elif kind == 'SoftClassProperty':
            d['cls'] = self.type_index('UClass')
            d['meta'] = class_of(inner_of('TSoftClassPtr<')) if text.startswith('TSoftClassPtr<') else self.type_index('UObject')
        elif kind == 'InterfaceProperty':
            if not text.startswith('TScriptInterface<'):
                return None
            d['cls'] = class_of(inner_of('TScriptInterface<'))
            if d['cls'] is None:
                return None
        elif kind == 'ArrayProperty':
            if not text.startswith('TArray<'):
                return None
            d['inner'] = self.inner_descriptor(inner_of('TArray<'))
            if d['inner'] is None:
                return None
        elif kind == 'SetProperty':
            if not text.startswith('TSet<'):
                return None
            d['elem'] = self.inner_descriptor(inner_of('TSet<'))
            if d['elem'] is None:
                return None
        elif kind == 'MapProperty':
            if not text.startswith('TMap<'):
                return None
            args = split_top_level(inner_of('TMap<'))
            if len(args) != 2:
                return None
            d['key'] = self.inner_descriptor(args[0])
            d['val'] = self.inner_descriptor(args[1])
            if d['key'] is None or d['val'] is None:
                return None
        elif kind in ('ByteProperty', 'EnumProperty'):
            if text not in NUMERIC:
                d['enum'] = self.type_index(text)
                if d['enum'] is None:
                    return None
                if kind == 'EnumProperty':
                    underlying = NUMERIC.get(self.enum_underlying.get(text, 'uint8'), NUMERIC['uint8'])
                    d['under'] = dict(k=underlying[0], s=underlying[1])
            elif kind == 'EnumProperty':
                return None
        elif kind in ('DelegateProperty', 'MulticastInlineDelegateProperty'):
            prefix = 'TDelegate<' if kind == 'DelegateProperty' else 'TMulticastInlineDelegate<'
            if not text.startswith(prefix):
                return None
            d['sig'] = self.signature(inner_of(prefix))
            if d['sig'] is None:
                return None
        elif kind == 'FieldPathProperty':
            d['field'] = inner_of('TFieldPath<')[len('class F'):] if text.startswith('TFieldPath<class F') else 'Property'
        elif kind == 'BoolProperty':
            pass
        return d

    def inner_descriptor(self, text):
        """Descriptor of a container element / delegate parameter, the property kind is derived from the C++ type"""
        text = text.strip()
        if text in NUMERIC:
            return dict(k=NUMERIC[text][0], s=NUMERIC[text][1])
        simple = {'bool': ('BoolProperty', 1), 'class FName': ('NameProperty', 8), 'class FString': ('StrProperty', 0x10), 'class FText': ('TextProperty', 0x18)}
        if text in simple:
            d = dict(k=simple[text][0], s=simple[text][1])
            if text == 'bool':
                d['bool'] = [1, 0, 1, 0xFF]
            return d
        for prefix, kind, size in (('TArray<', 'ArrayProperty', 0x10), ('TSet<', 'SetProperty', 0x50), ('TMap<', 'MapProperty', 0x50),
                                   ('TWeakObjectPtr<', 'WeakObjectProperty', 8), ('TLazyObjectPtr<', 'LazyObjectProperty', 0x1C),
                                   ('TSoftObjectPtr<', 'SoftObjectProperty', 0x28), ('TSoftClassPtr<', 'SoftClassProperty', 0x28),
                                   ('TSubclassOf<', 'ClassProperty', 8), ('TScriptInterface<', 'InterfaceProperty', 0x10),
                                   ('TDelegate<', 'DelegateProperty', 0x10), ('TMulticastInlineDelegate<', 'MulticastInlineDelegateProperty', 0x10),
                                   ('TFieldPath<', 'FieldPathProperty', 0x20)):
            if text.startswith(prefix):
                return self.descriptor(kind, text, size)
        if text.startswith('struct '):
            index = self.type_index(text[7:])
            if index is None:
                return None
            return dict(k='StructProperty', s=self.struct_info.get(index, {}).get('total', 0), struct=index)
        if text.startswith('class ') and text.endswith('*'):
            index = self.type_index(text[6:-1])
            if index is None:
                return None
            if text[6:-1].strip() == 'UClass':
                return dict(k='ClassProperty', s=8, cls=index, meta=self.type_index('UObject'))
            return dict(k='ObjectProperty', s=8, cls=index)
        fixup = re.match(r'^(?:class )?F(\w+Property)_$', text)
        if fixup:
            return dict(k=fixup.group(1), s=8)
        if text in self.enum_underlying:
            underlying = NUMERIC.get(self.enum_underlying[text], NUMERIC['uint8'])
            index = self.type_index(text)
            if index is None:
                return None
            if underlying[0] == 'ByteProperty':
                return dict(k='ByteProperty', s=1, enum=index)
            return dict(k='EnumProperty', s=underlying[1], enum=index, under=dict(k=underlying[0], s=underlying[1]))
        self.stats['unknown inner type'] += 1
        if len(self.examples['unknown inner type']) < 12:
            self.examples['unknown inner type'].append(text)
        return None

    def signature(self, text):
        """'void(class APawn* Pawn, const struct FVector& Location)' -> synthetic DelegateFunction parameters"""
        m = re.match(r'^(.*?)\((.*)\)$', text.strip(), re.S)
        if not m:
            return None
        params = []
        offset = 0
        for param in split_top_level(m.group(2)):
            pm = re.match(r'^(.*?)\s*([A-Za-z_]\w*)$', param)
            if not pm:
                return None
            type_text, name = pm.group(1).strip(), pm.group(2)
            flags = CPF_Parm
            if type_text.startswith('const '):
                type_text = type_text[6:].strip()
                flags |= CPF_ConstParm
            if type_text.endswith('&'):
                type_text = type_text[:-1].strip()
                flags |= CPF_ReferenceParm | (0 if flags & CPF_ConstParm else CPF_OutParm)
            elif type_text.endswith('*'):
                base = type_text[:-1].strip()
                if not (base.startswith('class ') and not base.endswith('*')):
                    type_text = base
                    flags |= CPF_OutParm
            d = self.inner_descriptor(type_text)
            if d is None:
                return None
            d['n'] = name
            d['f'] = d.get('f', 0) | flags
            d['o'] = offset
            offset += max(d['s'], 1)
            params.append(d)
        if m.group(1).strip() != 'void':
            d = self.inner_descriptor(m.group(1))
            if d is None:
                return None
            d.update(n='ReturnValue', f=CPF_Parm | CPF_OutParm | CPF_ReturnParm, o=offset)
            params.append(d)
        return params

    # ---------------------------------------------------------------- properties

    @staticmethod
    def valid_name(name):
        return re.sub(r'[^A-Za-z0-9_]', '_', name)

    def pair_members(self, index):
        props = self.props.get(index, [])
        info = self.struct_info.get(index)
        members = info['members'] if info else []

        by_offset = defaultdict(list)
        for m in members:
            by_offset[m['offset']].append(m)

        result = []
        for offset, kind, name in props:
            candidates = [m for m in by_offset.get(offset, []) if not m.get('used')]
            valid = self.valid_name(name)
            match = next((m for m in candidates if m['name'] == valid), None)
            if match is None:
                match = next((m for m in candidates if m['name'].startswith(valid) or valid.startswith(m['name'])), None)
            if match is None and len(candidates) == 1:
                match = candidates[0]
            if match is None:
                self.stats[f'unpaired {kind}'] += 1
                if len(self.examples[f'unpaired {kind}']) < 4:
                    self.examples[f'unpaired {kind}'].append((self.objects[index][1], offset, name, [(m['name'], m['offset']) for m in members][:6]))
                continue
            match['used'] = True

            size = match['size'] if not match['bitfield'] else 1  # for C-arrays the SDK comment has the element size
            d = self.descriptor(kind, match['type'], size)
            if d is None:
                self.stats[f'untyped {kind}'] += 1
                if len(self.examples[f'untyped {kind}']) < 6:
                    self.examples[f'untyped {kind}'].append((self.objects[index][1], name, match['type']))
                continue
            d.update(n=name, o=offset, d=match['dim'], f=d.get('f', 0) | match['flags'])
            if kind == 'BoolProperty':
                if match['bitfield']:
                    mask = 1 << (match['bit'] or 0)
                    d['bool'] = [1, 0, mask, mask]
                else:
                    d['bool'] = [1, 0, 1, 0xFF]
            result.append(d)
            self.stats['typed properties'] += 1
        return result

    # ---------------------------------------------------------------- output

    def build(self):
        self.read_objects()
        self.resolve_outers()
        self.resolve_classes()
        self.read_sdk()

        struct_kinds = {'Class', 'ScriptStruct', 'Function', 'DelegateFunction', 'SparseDelegateFunction', 'UserDefinedStruct'}
        objects, structs, functions, enums = [], {}, {}, {}
        for index, obj in enumerate(self.objects):
            if obj is None:
                objects.append(None)
                continue
            name = self.names[index]
            objects.append([self.class_index[index], self.outer[index], name, 0x11 if name.startswith('Default__') else 0x1])

            info = self.struct_info.get(index)
            is_struct = obj[0] in struct_kinds or obj[0].endswith('GeneratedClass') or info is not None
            if is_struct:
                super_index = -1
                super_cpp = info and (info['super_cpp'] or self.inherit.get(info['cpp']))
                if super_cpp:
                    super_index = self.type_index(super_cpp)
                    if super_index is None:
                        super_index = -1
                size = info['total'] if info else 0
                align = info['align'] if info else 1
                if info and info['packed']:
                    self.stats['packed (reused tail padding)'] += 1
                is_function = obj[0].endswith('Function')
                props = self.pair_members(index)
                if is_function and not info:
                    # Delegate signatures without a parameter struct in the SDK: only parameters whose type follows from their kind
                    props = [dict(k=k, n=n, o=o, s=NUMERIC[t][1], d=1, f=CPF_Parm) for o, k, n in self.props.get(index, [])
                             for t in [next((t for t, v in NUMERIC.items() if v[0] == k), None)] if t]
                structs[index] = [super_index, size, align, props]
                if is_function:
                    default = FUNCTION_FLAGS['Public'] | FUNCTION_FLAGS['Native']
                    if 'Delegate' in obj[0]:
                        default = FUNCTION_FLAGS['Public'] | FUNCTION_FLAGS['Delegate'] | FUNCTION_FLAGS['MulticastDelegate']
                    functions[index] = self.function_flags.get(index, default)
            if index in self.enum_info:
                enums[index] = self.enum_info[index][1]

        world = next((i for i, o in enumerate(self.objects) if o and o[0] == 'World' and not self.names[i].startswith('Default__')), -1)
        log('stats:', json.dumps(dict(sorted(self.stats.items())), indent=1))
        if os.environ.get('MAKE_WORLD_VERBOSE'):
            log('examples:', json.dumps(self.examples, indent=1))
        return dict(objects=objects, structs=structs, functions=functions, enums=enums, world=world)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    world = World(sys.argv[1]).build()
    with open(sys.argv[2], 'w') as f:
        json.dump(world, f, separators=(',', ':'))
    log(f'wrote {sys.argv[2]}: {len(world["objects"])} objects, {len(world["structs"])} structs, {len(world["enums"])} enums')


if __name__ == '__main__':
    main()
