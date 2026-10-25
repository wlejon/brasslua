#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BRASSLUA_BIN="${1:-${REPO_ROOT}/build/brasslua}"
LUAJIT_BIN="/usr/bin/luajit"
LUA54_BIN="/usr/bin/lua5.4"

if [[ ! -x "${BRASSLUA_BIN}" ]]; then
    echo "Error: brasslua binary not found at ${BRASSLUA_BIN}" >&2
    exit 1
fi

TMP_DIR="$(mktemp -d /tmp/brasslua_bench_XXXXXX)"
trap 'rm -rf "${TMP_DIR}"' EXIT

cat > "${TMP_DIR}/fib.lua" << 'EOF'
local function fib(n)
    if n < 2 then return n end
    return fib(n - 1) + fib(n - 2)
end
local t0 = os.clock()
local res = fib(37)
local dt = (os.clock() - t0) * 1000.0
print(string.format("RESULT:%d", res))
io.stderr:write(string.format("%.2f\n", dt))
EOF

cat > "${TMP_DIR}/harmonic.lua" << 'EOF'
local function harmonic(n)
    local sum = 0.0
    for i = 1.0, n, 1.0 do
        sum = sum + (1.0 / i)
    end
    return sum
end
local t0 = os.clock()
local res = harmonic(50000000.0)
local dt = (os.clock() - t0) * 1000.0
print(string.format("RESULT:%.9f", res))
io.stderr:write(string.format("%.2f\n", dt))
EOF

cat > "${TMP_DIR}/mandelbrot.lua" << 'EOF'
local function mandelbrot(N)
    local count = 0
    local invN = 2.0 / N
    for y = 0, N - 1 do
        local ci = y * invN - 1.0
        for x = 0, N - 1 do
            local cr = x * invN - 1.5
            local zr = 0.0
            local zi = 0.0
            local inside = 1
            for iter = 1, 50 do
                local tr = zr * zr - zi * zi + cr
                zi = 2.0 * zr * zi + ci
                zr = tr
                if zr * zr + zi * zi > 4.0 then
                    inside = 0
                    break
                end
            end
            count = count + inside
        end
    end
    return count
end
local t0 = os.clock()
local res = mandelbrot(600)
local dt = (os.clock() - t0) * 1000.0
print(string.format("RESULT:%d", res))
io.stderr:write(string.format("%.2f\n", dt))
EOF

cat > "${TMP_DIR}/spectral_norm.lua" << 'EOF'
local function A(i, j)
    local ij = i + j - 1
    return 1.0 / (ij * (ij - 1) * 0.5 + i)
end

local function Av(N, x, y)
    for i = 1, N do
        local a = 0.0
        for j = 1, N do
            a = a + A(i, j) * x[j]
        end
        y[i] = a
    end
end

local function Atv(N, x, y)
    for i = 1, N do
        local a = 0.0
        for j = 1, N do
            a = a + A(j, i) * x[j]
        end
        y[i] = a
    end
end

local function AtAv(N, x, y, t)
    Av(N, x, t)
    Atv(N, t, y)
end

local t0 = os.clock()
local N = 200
local u, v, t = {}, {}, {}
for i = 1, N do u[i] = 1.0; v[i] = 0.0; t[i] = 0.0 end
for iter = 1, 10 do
    AtAv(N, u, v, t)
    AtAv(N, v, u, t)
end
local vBv, vv = 0.0, 0.0
for i = 1, N do
    local ui = u[i]
    local vi = v[i]
    vBv = vBv + ui * vi
    vv = vv + vi * vi
end
local res = math.sqrt(vBv / vv)
local dt = (os.clock() - t0) * 1000.0
print(string.format("RESULT:%.9f", res))
io.stderr:write(string.format("%.2f\n", dt))
EOF

cat > "${TMP_DIR}/nbody.lua" << 'EOF'
local sqrt = math.sqrt
local b1 = { x = 0.0, y = 0.0, z = 0.0, vx = 0.0, vy = 0.0, vz = 0.0, mass = 39.47841760435743 }
local b2 = { x = 4.84143144246472, y = -1.16032004402742, z = -0.10362204447112, vx = 0.60632639299583, vy = 2.81198674491876, vz = -0.02521836165988, mass = 0.03769367487038 }
local b3 = { x = 8.34336671824458, y = 4.12479856412430, z = -0.40352341711432, vx = -1.01077434617879, vy = 1.82566237123041, vz = 0.00841576137658, mass = 0.01128632613196 }
local bodies = { b1, b2, b3 }

local function advance(dt, steps)
    for s = 1, steps do
        for i = 1, 3 do
            local bi = bodies[i]
            for j = i + 1, 3 do
                local bj = bodies[j]
                local dx = bi.x - bj.x
                local dy = bi.y - bj.y
                local dz = bi.z - bj.z
                local d2 = dx * dx + dy * dy + dz * dz
                local mag = dt / (d2 * sqrt(d2))
                local bmi = bi.mass * mag
                local bmj = bj.mass * mag
                bi.vx = bi.vx - dx * bmj
                bi.vy = bi.vy - dy * bmj
                bi.vz = bi.vz - dz * bmj
                bj.vx = bj.vx + dx * bmi
                bj.vy = bj.vy + dy * bmi
                bj.vz = bj.vz + dz * bmi
            end
        end
        for i = 1, 3 do
            local b = bodies[i]
            b.x = b.x + dt * b.vx
            b.y = b.y + dt * b.vy
            b.z = b.z + dt * b.vz
        end
    end
end

local t0 = os.clock()
advance(0.01, 400000)
local dt = (os.clock() - t0) * 1000.0
print(string.format("RESULT:%.9f", b1.x + b2.x + b3.x))
io.stderr:write(string.format("%.2f\n", dt))
EOF

cat > "${TMP_DIR}/sieve.lua" << 'EOF'
local function run_sieve(N, reps)
    local is_prime = {}
    local count = 0
    for r = 1, reps do
        count = 0
        for i = 1, N do is_prime[i] = 1 end
        for i = 2, 316 do
            if is_prime[i] == 1 then
                for k = i * i, N, i do
                    is_prime[k] = 0
                end
            end
        end
        for i = 2, N do
            count = count + is_prime[i]
        end
    end
    return count
end
local t0 = os.clock()
local res = run_sieve(50000, 40)
local dt = (os.clock() - t0) * 1000.0
print(string.format("RESULT:%d", res))
io.stderr:write(string.format("%.2f\n", dt))
EOF

cat > "${TMP_DIR}/fannkuch.lua" << 'EOF'
local function fannkuch(n)
    local p, q, s = {}, {}, {}
    for i = 1, n do p[i] = i; q[i] = i; s[i] = i end
    local sign = 1
    local maxflips = 0
    local sum = 0
    while true do
        local q1 = p[1]
        if q1 ~= 1 then
            for i = 2, n do q[i] = p[i] end
            local flips = 1
            while true do
                local qq = q[q1]
                if qq == 1 then
                    sum = sum + sign * flips
                    if flips > maxflips then maxflips = flips end
                    break
                end
                q[q1] = q1
                if q1 >= 4 then
                    local i, j = 2, q1 - 1
                    repeat
                        local t = q[i]; q[i] = q[j]; q[j] = t
                        i = i + 1; j = j - 1
                    until i >= j
                end
                q1 = qq
                flips = flips + 1
            end
        end
        if sign == 1 then
            p[2], p[1] = p[1], p[2]; sign = -1
        else
            p[2], p[3] = p[3], p[2]; sign = 1
            for i = 3, n do
                local sx = s[i]
                if sx ~= 1 then s[i] = sx - 1; break end
                if i == n then return sum, maxflips end
                s[i] = i
                local t = p[1]
                for j = 1, i do p[j] = p[j + 1] end
                p[i + 1] = t
            end
        end
    end
end
local t0 = os.clock()
local sum, flips = fannkuch(9)
local dt = (os.clock() - t0) * 1000.0
print(string.format("RESULT:%d:%d", sum, flips))
io.stderr:write(string.format("%.2f\n", dt))
EOF

cat > "${TMP_DIR}/bitops.lua" << 'EOF'
local bxor = bit.bxor
local band = bit.band
local lshift = bit.lshift
local rshift = bit.rshift
local function run_bits(n)
    local x = 123456789
    for i = 1, n do
        x = bxor(x, lshift(x, 13))
        x = bxor(x, rshift(x, 17))
        x = bxor(x, lshift(x, 5))
        x = band(x + i, 0x7fffffff)
    end
    return x
end
local t0 = os.clock()
local res = run_bits(20000000)
local dt = (os.clock() - t0) * 1000.0
print(string.format("RESULT:%d", res))
io.stderr:write(string.format("%.2f\n", dt))
EOF

cat > "${TMP_DIR}/matmul.lua" << 'EOF'
local N = 100
local A, B, C = {}, {}, {}
for i = 1, N * N do
    A[i] = (i % 17) * 0.125
    B[i] = (i % 13) * 0.25
    C[i] = 0.0
end
local t0 = os.clock()
for i = 0, N - 1 do
    local rowA = i * N
    for k = 0, N - 1 do
        local aik = A[rowA + k + 1]
        local rowB = k * N
        for j = 1, N do
            local idx = rowA + j
            C[idx] = C[idx] + aik * B[rowB + j]
        end
    end
end
local dt = (os.clock() - t0) * 1000.0
print(string.format("RESULT:%.6f", C[1] + C[N * N]))
io.stderr:write(string.format("%.2f\n", dt))
EOF

printf "%-16s | %12s | %12s | %12s | %s\n" "Benchmark" "brasslua(ms)" "luajit(ms)" "lua5.4(ms)" "Oracle"
printf "%-16s-+-%12s-+-%12s-+-%12s-+-%s\n" "----------------" "------------" "------------" "------------" "------"

BENCHES=(fib harmonic mandelbrot spectral_norm nbody sieve fannkuch bitops matmul)

for b in "${BENCHES[@]}"; do
    f="${TMP_DIR}/${b}.lua"
    bl_out="$("${BRASSLUA_BIN}" "${f}" 2>"${TMP_DIR}/${b}.bl.time")"
    bl_ms="$(cat "${TMP_DIR}/${b}.bl.time")"

    lj_out="$("${LUAJIT_BIN}" "${f}" 2>"${TMP_DIR}/${b}.lj.time")"
    lj_ms="$(cat "${TMP_DIR}/${b}.lj.time")"

    l54_ms="N/A"
    if [[ "${b}" != "bitops" ]] && [[ -x "${LUA54_BIN}" ]]; then
        l54_out="$("${LUA54_BIN}" "${f}" 2>"${TMP_DIR}/${b}.l54.time")"
        l54_ms="$(cat "${TMP_DIR}/${b}.l54.time")"
    else
        l54_out="${lj_out}"
    fi

    if [[ "${bl_out}" == "${lj_out}" ]] && [[ "${bl_out}" == "${l54_out}" ]]; then
        status="PASS"
    else
        status="MISMATCH (bl=${bl_out} lj=${lj_out} l54=${l54_out})"
    fi

    printf "%-16s | %12s | %12s | %12s | %s\n" "${b}" "${bl_ms}" "${lj_ms}" "${l54_ms}" "${status}"
done
