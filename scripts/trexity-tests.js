#!/usr/bin/env node

/*
 Simple Node.js test runner for VROOM CLI.
 Mirrors tests from scripts/trexity-tests.sh including pinned semantics.
*/

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

function findBinary() {
  const envBin = process.env.VROOM_BIN;
  if (envBin && fs.existsSync(envBin)) return envBin;
  const root = path.resolve(__dirname, '..');
  const macos = path.join(root, 'bin', 'vroom-macos');
  const linux = path.join(root, 'bin', 'vroom');
  if (fs.existsSync(macos)) return macos;
  if (fs.existsSync(linux)) return linux;
  throw new Error('FATAL: No vroom binary found. Set VROOM_BIN or build first.');
}

const BIN = findBinary();

function runVroom(inputPath) {
  const res = spawnSync(BIN, ['-i', inputPath], { encoding: 'utf8' });
  const stdout = res.stdout || '';
  const code = res.status ?? 1;
  let json;
  try {
    json = JSON.parse(stdout || '{}');
  } catch (_) {
    json = {};
  }
  return { code, json, stdout };
}

function tmpDir() {
  // Ensure base tmp exists even if process.env.TMPDIR points to a removed dir
  let base = os.tmpdir();
  try {
    if (!fs.existsSync(base)) {
      fs.mkdirSync(base, { recursive: true });
    }
  } catch (_) {
    base = '/tmp';
  }
  if (!fs.existsSync(base)) base = '/tmp';
  return fs.mkdtempSync(path.join(base, 'trexity-vroom-node-tests.'));
}

function writeJSON(tmp, name, obj) {
  const p = path.join(tmp, name);
  fs.writeFileSync(p, JSON.stringify(obj));
  return p;
}

function matrix2() {
  return { car: { durations: [[0, 1000], [1000, 0]] } };
}
function matrix2_500() {
  return { car: { durations: [[0, 500], [500, 0]] } };
}
function matrix3_200() {
  return { car: { durations: [[0, 200, 200], [200, 0, 200], [200, 200, 0]] } };
}

// Skewed 3x3 matrix to make the optimal order Start -> Job2 -> Job1
// cheaper than Start -> Job1 -> Job2.
function matrix3_skewed() {
  // indices: 0=start, 1=job1, 2=job2
  // 0->2 and 2->1 are cheap; 0->1 is very expensive, so solver should place job2 before job1
  return { car: { durations: [[0, 1000, 100], [1000, 0, 100], [100, 100, 0]] } };
}

function assertExit(exp, got) {
  if (exp !== got) throw new Error(`Expected exit ${exp}, got ${got}`);
}
function assertJsonEq(obj, jqPath, expected) {
  const parts = jqPath.replace(/^\./, '').split('.');
  let cur = obj;
  for (const p of parts) {
    if (p === '') continue;
    if (!(p in cur)) throw new Error(`JSON path not found: ${jqPath}`);
    cur = cur[p];
  }
  if (String(cur) !== String(expected)) {
    throw new Error(`Assert ${jqPath} == ${expected} failed (got ${cur})`);
  }
}

async function run(name, fn) {
  process.stdout.write(`[TEST] ${name}\n`);
  try {
    await fn();
    process.stdout.write('  PASS\n');
    return true;
  } catch (e) {
    process.stdout.write(`  FAIL: ${e.message}\n`);
    return false;
  }
}

// ----------------------- Tests -----------------------

const tests = {
  async job_allowed_unassigned() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0 }],
      jobs: [{ id: 1, location_index: 1, allowed_vehicles: [999] }],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'job_allowed_unassigned.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 1);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async job_required_fail() {
    // Legacy test removed since required never existed now. Keep as no-op pass.
  },

  async job_pinned_allowed_success() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, steps: [
        { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' }
      ] }],
      jobs: [{ id: 1, location_index: 1, pinned: true, allowed_vehicles: [101] }],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'job_pinned_allowed_success.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    assertJsonEq(json, '.routes.0.vehicle', 101);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async shipment_allowed_unassigned() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, capacity: [1] }],
      shipments: [{ amount: [1], allowed_vehicles: [999],
        pickup: { id: 9001, location_index: 0 },
        delivery: { id: 9002, location_index: 1 }
      }],
      matrices: matrix2_500()
    };
    const f = writeJSON(t, 'shipment_allowed_unassigned.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 2);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async shipment_required_fail() {
    // Legacy test removed since required never existed now. Keep as no-op pass.
  },

  async shipment_pinned_allowed_success() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, capacity: [1], steps: [
        { type: 'start' }, { type: 'pickup', id: 9001 }, { type: 'delivery', id: 9002 }, { type: 'end' }
      ] }],
      shipments: [{ amount: [1], pinned: true, allowed_vehicles: [101],
        pickup: { id: 9001, location_index: 0 },
        delivery: { id: 9002, location_index: 1 }
      }],
      matrices: matrix2_500()
    };
    const f = writeJSON(t, 'shipment_pinned_allowed_success.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async skills_and_allowed_ok() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, skills: [1], steps: [
        { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' }
      ] }],
      jobs: [{ id: 1, location_index: 1, skills: [1], pinned: true, allowed_vehicles: [101] }],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'skills_allowed_ok.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async skills_and_allowed_fail() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, skills: [1], steps: [
        { type: 'start' }, { type: 'job', id: 2 }, { type: 'end' }
      ] }],
      jobs: [{ id: 2, location_index: 1, skills: [2], pinned: true, allowed_vehicles: [101] }],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'skills_allowed_fail.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async required_vehicle_deprecated_fail() {
    // Legacy test removed since required_vehicle never existed now. Keep as no-op pass.
  },

  async pinned_job_stays_same_vehicle() {
    const t = tmpDir();
    const input = {
      vehicles: [
        { id: 101, start_index: 0, steps: [ { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' } ] },
        { id: 102, start_index: 0 }
      ],
      jobs: [ { id: 1, location_index: 1, pinned: true }, { id: 2, location_index: 2 } ],
      matrices: matrix3_200()
    };
    const f = writeJSON(t, 'pinned_job_stays_same_vehicle.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    assertJsonEq(json, '.routes.length', 1);
    assertJsonEq(json, '.routes.0.vehicle', 101);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_job_missing_in_steps_err() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0 }],
      jobs: [{ id: 1, location_index: 1, pinned: true }],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'pinned_job_missing_in_steps_err.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_job_in_two_vehicles_err() {
    const t = tmpDir();
    const input = {
      vehicles: [
        { id: 101, start_index: 0, steps: [ { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' } ] },
        { id: 102, start_index: 0, steps: [ { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' } ] }
      ],
      jobs: [ { id: 1, location_index: 1, pinned: true } ],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'pinned_job_in_two_vehicles_err.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_job_allowed_conflict_err() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, steps: [ { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' } ] }],
      jobs: [{ id: 1, location_index: 1, pinned: true, allowed_vehicles: [999] }],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'pinned_job_allowed_conflict_err.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_shipment_same_vehicle() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, capacity: [1], steps: [
        { type: 'start' }, { type: 'pickup', id: 9001 }, { type: 'delivery', id: 9002 }, { type: 'end' }
      ] }],
      shipments: [{ amount: [1], pinned: true,
        pickup: { id: 9001, location_index: 0 },
        delivery: { id: 9002, location_index: 1 }
      }],
      matrices: matrix2_500()
    };
    const f = writeJSON(t, 'pinned_shipment_same_vehicle.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_shipment_split_err() {
    const t = tmpDir();
    const input = {
      vehicles: [
        { id: 101, start_index: 0, capacity: [1], steps: [ { type: 'start' }, { type: 'pickup', id: 9001 }, { type: 'end' } ] },
        { id: 102, start_index: 0, capacity: [1], steps: [ { type: 'start' }, { type: 'delivery', id: 9002 }, { type: 'end' } ] }
      ],
      shipments: [{ amount: [1], pinned: true,
        pickup: { id: 9001, location_index: 0 },
        delivery: { id: 9002, location_index: 1 }
      }],
      matrices: matrix2_500()
    };
    const f = writeJSON(t, 'pinned_shipment_split_err.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_infeasible_capacity_err() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, capacity: [0], steps: [
        { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' }
      ] }],
      jobs: [{ id: 1, location_index: 1, pinned: true, pickup: [1] }],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'pinned_infeasible_capacity_err.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  }
};

// ---------------- Additional Edge Case Tests ----------------
Object.assign(tests, {
  async pinned_job_reorder_with_added_task_success() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, steps: [
        { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' }
      ] }],
      jobs: [
        { id: 1, location_index: 1, pinned: true },
        { id: 2, location_index: 2 }
      ],
      matrices: matrix3_skewed()
    };
    const f = writeJSON(t, 'pinned_job_reorder_with_added_task_success.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    assertJsonEq(json, '.routes.length', 1);
    assertJsonEq(json, '.routes.0.vehicle', 101);
    // Expect the solver to reorder due to strong skew: Job2 before Job1
    const steps = json.routes[0].steps;
    const jobSteps = steps.filter(s => s.type === 'job');
    if (jobSteps.length < 2) throw new Error('Expected 2 job steps in route');
    if (jobSteps[0].id !== 2 || jobSteps[1].id !== 1) {
      throw new Error(`Expected order [2,1], got [${jobSteps.map(s => s.id)}]`);
    }
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_two_jobs_same_vehicle_success() {
    const t = tmpDir();
    const input = {
      vehicles: [
        { id: 101, start_index: 0, steps: [ { type: 'start' }, { type: 'job', id: 1 }, { type: 'job', id: 2 }, { type: 'end' } ] },
        { id: 102, start_index: 0 }
      ],
      jobs: [
        { id: 1, location_index: 1, pinned: true },
        { id: 2, location_index: 2, pinned: true }
      ],
      matrices: matrix3_200()
    };
    const f = writeJSON(t, 'pinned_two_jobs_same_vehicle_success.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    assertJsonEq(json, '.routes.length', 1);
    assertJsonEq(json, '.routes.0.vehicle', 101);
    const steps = json.routes[0].steps;
    const jobIds = steps.filter(s => s.type === 'job').map(s => s.id).sort((a,b)=>a-b);
    if (String(jobIds) !== String([1,2])) {
      throw new Error(`Expected job steps [1,2] on vehicle 101, got [${jobIds}]`);
    }
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_shipment_partial_steps_err() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, capacity: [1], steps: [
        { type: 'start' }, { type: 'pickup', id: 9001 }, { type: 'end' }
      ] }],
      shipments: [{ amount: [1], pinned: true,
        pickup: { id: 9001, location_index: 0 },
        delivery: { id: 9002, location_index: 1 }
      }],
      matrices: matrix2_500()
    };
    const f = writeJSON(t, 'pinned_shipment_partial_steps_err.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_job_plus_unpinned_assigned_success() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, steps: [
        { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' }
      ] }],
      jobs: [
        { id: 1, location_index: 1, pinned: true },
        { id: 2, location_index: 2 }
      ],
      matrices: matrix3_200()
    };
    const f = writeJSON(t, 'pinned_job_plus_unpinned_assigned_success.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    assertJsonEq(json, '.routes.length', 1);
    assertJsonEq(json, '.routes.0.vehicle', 101);
    const steps = json.routes[0].steps;
    const jobIds = steps.filter(s => s.type === 'job').map(s => s.id).sort((a,b)=>a-b);
    if (String(jobIds) !== String([1,2])) {
      throw new Error(`Expected both jobs [1,2] on vehicle 101, got [${jobIds}]`);
    }
    fs.rmSync(t, { recursive: true, force: true });
  }
});

async function main() {
  const order = [
    'job_allowed_unassigned',
    'job_required_fail',
    'job_pinned_allowed_success',
    'shipment_allowed_unassigned',
    'shipment_required_fail',
    'shipment_pinned_allowed_success',
    'skills_and_allowed_ok',
    'skills_and_allowed_fail',
    'required_vehicle_deprecated_fail',
    'pinned_job_stays_same_vehicle',
    'pinned_job_missing_in_steps_err',
    'pinned_job_in_two_vehicles_err',
    'pinned_job_allowed_conflict_err',
    'pinned_shipment_same_vehicle',
    'pinned_shipment_split_err',
    'pinned_infeasible_capacity_err',
    // Additional edge cases
    'pinned_job_reorder_with_added_task_success',
    'pinned_two_jobs_same_vehicle_success',
    'pinned_shipment_partial_steps_err',
    'pinned_job_plus_unpinned_assigned_success'
  ];

  let pass = 0, fail = 0;
  for (const name of order) {
    // eslint-disable-next-line no-await-in-loop
    const ok = await run(name, tests[name]);
    if (ok) pass++; else fail++;
  }
  process.stdout.write(`\nSummary: ${pass} passed, ${fail} failed\n`);
  process.exit(fail === 0 ? 0 : 1);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});


