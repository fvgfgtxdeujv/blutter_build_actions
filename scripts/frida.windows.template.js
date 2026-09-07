// blutter Frida script for Flutter Windows desktop (x86-64, uncompressed tagged pointers).
// Attach to the running app, e.g.:
//   frida -p <PID> -l blutter_frida_windows.js
// Hook offsets come from blutter asm/ output (fn_<hex>). The AOT image base is
// auto-detected from CodeAnchors; if detection fails, call setBase("0x...") manually.
//
// Config constants (PointerCompressedEnabled, TaggedWordSize, CodeAnchors,
// ClassIdTagPos, ClassIdTagMask, NumPredefinedCids, Cid*, Classes) are appended
// by blutter FridaWriter after this template body.

const ShowNullField = false;
const MaxDepth = 5;
var imageBase = null;
let hooksDone = false;

function u64ToBig(val) {
    return BigInt('0x' + val.toString(16).replace(/^0x/i, ''));
}

function wordBigAt(ptr, off) {
    return u64ToBig(ptr.add(off).readU64());
}

function smiUntag(v) {
    if (v >= 2n ** 63n)
        v -= 2n ** 64n;
    return v >> 1n;
}

function fmtSmi(v) {
    if (v >= -(2n ** 53n) && v <= 2n ** 53n)
        return Number(v);
    return v.toString();
}

function init(context) {
    // no-op on x64: tagged pointers are absolute, no heap base needed
}

function getArg(context, idx) {
    // Dart x64 convention: fixed args pushed by caller on stack;
    // at function entry [rsp] is the return address, arg i at [rsp + 8 + 8*i]
    return context.rsp.add(8 + 8 * idx).readPointer();
}

function isHeapObject(ptr) {
    return (ptr.toInt32() & 1) == 1;
}

function getObjectCid(ptr) {
    const tag = ptr.readU32();
    return (tag >> ClassIdTagPos) & ClassIdTagMask;
}

function getDartBool(ptr, cls) {
    return ptr.add(cls.valOffset).readU8() != 0;
}

function getDartMint(ptr, cls) {
    return wordBigAt(ptr, cls.valOffset);
}

function getDartDouble(ptr, cls) {
    return ptr.add(cls.valOffset).readDouble();
}

function getDartString(ptr, cls) {
    // Dart stores string length as Smi (64-bit word on x64)
    const len = fmtSmi(smiUntag(wordBigAt(ptr, cls.lenOffset)));
    return ptr.add(cls.dataOffset).readUtf8String(len);
}

function getDartTwoByteString(ptr, cls) {
    const len = fmtSmi(smiUntag(wordBigAt(ptr, cls.lenOffset)));
    return ptr.add(cls.dataOffset).readUtf16String(len * 2);
}

function getDartArray(ptr, cls, depthLeft, glen = null) {
    const rawLen = glen === null ? fmtSmi(smiUntag(wordBigAt(ptr, cls.lenOffset))) : glen;
    const len = typeof rawLen === 'number' ? rawLen : Number(rawLen);
    let vals = [];
    let dataPtr = ptr.add(cls.dataOffset);
    for (let i = 0; i < len; i++) {
        let dptr = dataPtr.add(i * TaggedWordSize).readPointer();
        const [tptr, ocls, fieldValue] = getTaggedObjectValue(dptr, depthLeft - 1);
        if ([CidNull, CidSmi, CidMint, CidDouble, CidBool, CidString, CidTwoByteString].includes(ocls.id)) {
            vals.push(fieldValue);
        }
        else {
            const key = `${ocls.name}@${tptr.toString().slice(2)}`;
            vals.push({ key: fieldValue });
        }
    }
    return vals;
}

function getDartGrowableArray(ptr, cls, depthLeft) {
    const len = fmtSmi(smiUntag(wordBigAt(ptr, cls.lenOffset)));
    let arrPtr = ptr.add(cls.dataOffset);
    return getDartArray(arrPtr, Classes[CidArray], depthLeft, len);
}

function getDartTypedArrayValues(ptr, cls, elementSize, readValFn) {
    const len = fmtSmi(smiUntag(wordBigAt(ptr, cls.lenOffset)));
    let dataPtr = ptr.add(cls.dataOffset);
    let vals = [];
    for (let i = 0; i < Number(len); i++) {
        let val = readValFn(dataPtr.add(i * elementSize));
        vals.push(val);
    }
    return vals;
}

function getDartClosure(ptr, cls) {
    let ep = ptr.add(cls.epOffset).readPointer();
    let fnName = 'unknown';
    if (imageBase !== null) {
        let offset = ep.sub(imageBase);
        fnName = 'fn_' + offset.toString(16);
    }
    return `Closure(${fnName})`;
}

function getDartLinkedHashData(ptr, cls, depthLeft, isMap) {
    const usedData = fmtSmi(smiUntag(wordBigAt(ptr, cls.usedOffset)));
    let arrPtr = ptr.add(cls.dataOffset);
    let dataCls = Classes[CidArray];
    const n = Number(usedData);

    if (isMap) {
        let result = {};
        for (let i = 0; i < n; i += 2) {
            try {
                let keyPtr = arrPtr.add(dataCls.dataOffset + i * TaggedWordSize).readPointer();
                let valPtr = arrPtr.add(dataCls.dataOffset + (i + 1) * TaggedWordSize).readPointer();
                const [kTptr, kCls, kVal] = getTaggedObjectValue(keyPtr, depthLeft - 1);
                const [vTptr, vCls, vVal] = getTaggedObjectValue(valPtr, depthLeft - 1);
                if (kCls.id === CidNull) continue;
                let keyStr = (kCls.id === CidString || kCls.id === CidTwoByteString) ? kVal : `${kCls.name}@${kTptr.toString().slice(2)}`;
                result[keyStr] = vVal;
            } catch (e) { break; }
        }
        return result;
    } else {
        let items = [];
        for (let i = 0; i < n; i++) {
            try {
                let valPtr = arrPtr.add(dataCls.dataOffset + i * TaggedWordSize).readPointer();
                const [vTptr, vCls, vVal] = getTaggedObjectValue(valPtr, depthLeft - 1);
                if (vCls.id === CidNull) continue;
                items.push(vVal);
            } catch (e) { break; }
        }
        return items;
    }
}

function getDartMap(ptr, cls, depthLeft) {
    return getDartLinkedHashData(ptr, cls, depthLeft, true);
}

function getDartSet(ptr, cls, depthLeft) {
    return getDartLinkedHashData(ptr, cls, depthLeft, false);
}

function isFieldNative(fieldBitmap, offset) {
    const idx = offset / TaggedWordSize;
    return (fieldBitmap & (1 << idx)) !== 0;
}

function getObjectValue(ptr, cls, depthLeft = MaxDepth) {
    switch (cls.id) {
    case CidObject:
        console.error(`Object cid should not reach here`);
        return;
    case CidNull:
        return null;
    case CidBool:
        return getDartBool(ptr, cls);
    case CidString:
        return getDartString(ptr, cls);
    case CidTwoByteString:
        return getDartTwoByteString(ptr, cls);
    case CidMint:
        return getDartMint(ptr, cls);
    case CidDouble:
        return getDartDouble(ptr, cls);
    case CidArray:
        return getDartArray(ptr, cls, depthLeft);
    case CidGrowableArray:
        return getDartGrowableArray(ptr, cls, depthLeft);
    case CidUint8Array:
        return getDartTypedArrayValues(ptr, cls, 1, (p) => p.readU8());
    case CidInt8Array:
        return getDartTypedArrayValues(ptr, cls, 1, (p) => p.readS8());
    case CidUint16Array:
        return getDartTypedArrayValues(ptr, cls, 2, (p) => p.readU16());
    case CidInt16Array:
        return getDartTypedArrayValues(ptr, cls, 2, (p) => p.readS16());
    case CidUint32Array:
        return getDartTypedArrayValues(ptr, cls, 4, (p) => p.readU32());
    case CidInt32Array:
        return getDartTypedArrayValues(ptr, cls, 4, (p) => p.readS32());
    case CidUint64Array:
        return getDartTypedArrayValues(ptr, cls, 8, (p) => p.readU64());
    case CidInt64Array:
        return getDartTypedArrayValues(ptr, cls, 8, (p) => p.readS64());
    case CidClosure:
        return getDartClosure(ptr, cls);
    case CidSet:
        return getDartSet(ptr, cls, depthLeft);
    case CidMap:
        return getDartMap(ptr, cls, depthLeft);
    }

    if (cls.id < NumPredefinedCids) {
        const msg = `Unhandle class id: ${cls.id}, ${cls.name}`;
        console.log(msg);
        return msg;
    }

    if (depthLeft <= 0) {
        return 'no more recursive';
    }

    let parents = [];
    let scls = Classes[cls.sid];
    while (scls !== undefined && scls.id != CidObject) {
        parents.push(scls);
        scls = Classes[scls.sid];
    }
    let values = {};
    while (parents.length > 0) {
        const sscls = scls;
        scls = parents.pop();
        const parentValue = getInstanceValue(ptr, scls, sscls, depthLeft);
        values[`parent!${scls.name}`] = parentValue;
    }
    const myValue = getInstanceValue(ptr, cls, scls, depthLeft);
    Object.assign(values, myValue);
    return values;
}

function getInstanceValue(ptr, cls, scls, depthLeft = MaxDepth) {
    let values = {};
    let offset = scls.size;
    while (offset < cls.size) {
        if (offset == cls.argOffset) {
            // TODO: type arguments
            offset += TaggedWordSize;
        }
        else if (isFieldNative(cls.fbitmap, offset)) {
            // untagged raw slot (double / int64 on x64), 8 bytes, one bitmap bit
            const w = wordBigAt(ptr, offset);
            // int-vs-double heuristic mirrors blutter's Android template
            if (w <= 0x1000000000000000n || w >= 0xffffffffffff0000n) {
                values[`off_${offset.toString(16)}`] = fmtSmi(w <= 2n ** 62n ? w : w - (2n ** 64n));
            }
            else {
                values[`off_${offset.toString(16)}`] = ptr.add(offset).readDouble();
            }
            offset += TaggedWordSize;
        }
        else {
            // object
            let dptr = ptr.add(offset).readPointer();
            const [tptr, ocls, fieldValue] = getTaggedObjectValue(dptr, depthLeft - 1);
            if (ocls.id === CidSmi) {
                values[`off_${offset.toString(16)}!Smi`] = fieldValue;
            }
            else if (ocls.id !== CidNull) {
                values[`off_${offset.toString(16)}!${ocls.name}@${tptr.toString().slice(2)}`] = fieldValue;
            }
            else if (ShowNullField) {
                values[`off_${offset.toString(16)}`] = fieldValue;
            }
            offset += TaggedWordSize;
        }
    }

    return values;
}

// tptr (tagged pointer) is only for tagged object; on x64 pointers are absolute
// return format: [tptr, cls, values]
function getTaggedObjectValue(tptr, depthLeft = MaxDepth) {
    if (!isHeapObject(tptr)) {
        // smi: 64-bit tagged word, value = arithmetic >> 1
        return [tptr, Classes[CidSmi], fmtSmi(smiUntag(BigInt(tptr.toString(16))))];
    }

    let ptr = tptr.sub(1);
    const cid = getObjectCid(ptr);
    if (cid >= Classes.length || Classes[cid] === undefined)
        return [tptr, { id: cid, name: `UnknownCid#${cid}` }, 'unreadable'];
    const cls = Classes[cid];
    const values = getObjectValue(ptr, cls, depthLeft);
    return [tptr, cls, values];
}

// -------- image base discovery --------
function scanRanges(pattern) {
    let hits = [];
    for (const range of Process.enumerateRanges('r-x')) {
        try {
            for (const m of Memory.scanSync(range.base, range.size, pattern))
                hits.push(m.address);
        } catch (e) { /* skip unreadable range */ }
    }
    return hits;
}

function computeBase() {
    const votes = new Map(); // candidate base string -> count
    for (const [off, pattern] of CodeAnchors) {
        for (const hit of scanRanges(pattern)) {
            const cand = hit.sub(off).toString(16);
            votes.set(cand, (votes.get(cand) || 0) + 1);
        }
    }
    let best = null, bestCnt = 0;
    for (const [cand, cnt] of votes) {
        if (cnt > bestCnt) { bestCnt = cnt; best = cand; }
    }
    const need = CodeAnchors.length >= 4 ? 3 : 2;
    if (best !== null && bestCnt >= need)
        return ptr(best);
    return null;
}

function onBaseFound() {
    if (hooksDone) return;
    hooksDone = true;
    console.log('AOT image base detected: ' + imageBase);

    // Edit the hook below: replace fn_addr with the fn_<hex> offset you want,
    // remove the xxx() line, then re-run.
    xxx("remove this line and correct the hook value");
    const fn_addr = 0xdeadbeef;
    Interceptor.attach(imageBase.add(fn_addr), {
        onEnter: function () {
            init(this.context);
            let objPtr = getArg(this.context, 0);
            const [tptr, cls, values] = getTaggedObjectValue(objPtr);
            console.log(`${cls.name}@${tptr.toString().slice(2)} =`, JSON.stringify(values, null, 2));
        }
    });
}

function setBase(addr) {
    imageBase = typeof addr === 'string' ? ptr(addr) : addr;
    onBaseFound();
}

function tryFindBase() {
    const b = computeBase();
    if (b === null) {
        setTimeout(tryFindBase, 700);
    } else {
        imageBase = b;
        onBaseFound();
    }
}
tryFindBase();
