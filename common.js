/* Shared front-end code for the Railway Portal and the two partner sites.
   Every site talks to the SAME C backend (main.c): partner bookings are stored
   in the main database and can be read back by the main website. */
const RUPEE = "\u20B9";
const CLASSES = {
  "1A": { name: "First AC (1AC)",  fare: 3200, prefs: [["SB", "Single berth"]] },
  "2A": { name: "Second AC (2AC)", fare: 1900, prefs: [["LB", "Lower berth"], ["UB", "Upper berth"], ["SL", "Side Lower"], ["SU", "Side Upper"]] },
  "3A": { name: "Third AC (3AC)",  fare: 1250, prefs: [["LB", "Lower berth"], ["MB", "Middle berth"], ["UB", "Upper berth"], ["SL", "Side Lower"], ["SU", "Side Upper"]] },
};
const SITE = window.SITE || { channel: "DIRECT", key: "", name: "Railway Portal", discount: 0 };
const $ = (s, el = document) => el.querySelector(s);
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
const rs = (n) => RUPEE + Number(n).toLocaleString("en-IN");

async function api(path, opts) {
  try { const r = await fetch(path, opts); return await r.json(); }
  catch (e) { return { ok: false, message: "Cannot reach the server. Is ./railway serve running?" }; }
}
function post(path, obj) {
  return api(path, { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: new URLSearchParams(obj).toString() });
}
function initTabs() {
  document.querySelectorAll("#tabs button").forEach((b) => b.addEventListener("click", () => {
    document.querySelectorAll("#tabs button").forEach((x) => x.classList.toggle("on", x === b));
    document.querySelectorAll("section.tab").forEach((s) => s.classList.toggle("on", s.id === "tab-" + b.dataset.tab));
  }));
}
function header(active) {
  const links = [["DIRECT", "/", "Railway Portal"], ["MMT", "/makemytrip", "MakeMyTrip"], ["GOI", "/goibibo", "Goibibo"]];
  return links.map((l) => `<a href="${l[1]}" class="${l[0] === active ? "on" : ""}">${l[2]}</a>`).join("");
}
/* ---------------- shared helpers for dates / meta ---------------- */
let META = null;
async function getMeta() { if (!META || !META.stations) META = await api("/api/meta"); return META; }
function addDays(s, n) { const d = new Date(s + "T00:00:00Z"); d.setUTCDate(d.getUTCDate() + n); return d.toISOString().slice(0, 10); }
function fmtDay(s) { return new Date(s.slice(0, 10) + "T00:00:00").toLocaleDateString("en-IN", { weekday: "short", day: "2-digit", month: "short" }); }
function fmtDT(s) { return fmtDay(s) + ", " + s.slice(11, 16); }

function ticketHTML(b) {
  const cls = b.count === 1 ? "single" : b.together ? "together" : "split";
  const meal = (s) => s.mealId ? `${esc(s.meal)} <small class="muted">(${rs(s.mealPrice)})</small>` : `<span class="muted">No meal</span>`;
  const rows = b.names.map((n, i) => `<tr><td>${i + 1}</td><td>${esc(n)}</td><td><b>${esc(b.seats[i].label)}</b></td><td>${esc(b.seats[i].typeName)} berth</td><td>${meal(b.seats[i])}</td></tr>`).join("");
  let refund = "";
  if (b.status === "CANCELLED")
    refund = `<div class="note single">Cancelled ${esc(b.cancelledAt)}. Cancellation charge ${rs(b.charge)} &middot; <b>Refund ${rs(b.refund)}</b>${b.channel !== "DIRECT" ? " (credited via " + esc(b.channelName) + ")" : ""}</div>`;
  return `<div class="ticket">
    <div class="top"><div><div class="pnr">PNR ${b.pnr}</div><small class="muted">Booked ${esc(b.bookedAt)}</small></div>
    <div><span class="badge ${b.channel}">${esc(b.channelName)}</span> <span class="badge ${b.status}">${b.status}</span> <span class="badge p${b.priority}">Priority ${b.priority}</span></div></div>
    <p><b>Train ${b.trainNo} ${esc(b.trainName)}</b> &middot; ${esc(b.className)}</p>
    <div class="journey"><div><small>From</small><b>${esc(b.fromName)}</b><span>${fmtDT(b.dep)}</span></div><div class="arrow">&rarr;<small>${b.km} km</small></div><div><small>To</small><b>${esc(b.toName)}</b><span>${fmtDT(b.arr)}</span></div></div>
    <div class="note ${cls}">${cls === "split" ? "&#9888; " : ""}${esc(b.note)}</div>
    <div class="scroll"><table><tr><th>#</th><th>Passenger</th><th>Seat (coach/berth)</th><th>Berth type</th><th>Meal at seat</th></tr>${rows}</table></div>
    <div class="fare"><span>Fare: <b>${rs(b.fare)}</b></span>${b.discountPct ? `<span>Discount (${b.discountPct}%): <b>-${rs(b.discount)}</b></span>` : ""}${b.mealTotal ? `<span>Meals: <b>${rs(b.mealTotal)}</b></span>` : ""}<span>Amount paid: <b>${rs(b.paid)}</b></span></div>
    ${refund}</div>`;
}

/* ---------------- booking: search -> train -> passengers -> seats & meals -> ticket ---------------- */
function mountBooking(el) {
  let M = null, q = null, results = null, sel = null, held = null, timer = null, mealPick = [];
  const STEPS = ["Search trains", "Choose train", "Passengers & berth", "Seats & meals", "Ticket"];
  el.innerHTML = `<div class="steps" id="bk-steps"></div><div id="bk-search"></div><div id="bk-results"></div><div id="bk-pax"></div><div id="bk-meals"></div><div id="bk-done"></div>`;
  const S = (id) => $("#bk-" + id, el);
  const step = (n) => { S("steps").innerHTML = STEPS.map((s, i) => `<span class="${i + 1 < n ? "done" : i + 1 === n ? "cur" : ""}">${i + 1}. ${s}</span>`).join(""); };
  const stop = () => { if (timer) { clearInterval(timer); timer = null; } };
  const hm = (s) => s.slice(11, 16);
  const base = () => ({ channel: SITE.channel, key: SITE.key });

  window.addEventListener("beforeunload", () => {            /* leaving the page frees the held seats at once */
    if (held && navigator.sendBeacon) navigator.sendBeacon("/api/release", new URLSearchParams({ ...base(), pnr: held.pnr }));
  });

  async function init() {
    M = await getMeta();
    if (!M.stations) { S("search").innerHTML = `<div class="msg err">Cannot reach the server. Is ./railway serve running?</div>`; return; }
    q = { from: "MAS", to: "NDLS", date: addDays(M.today, 1) };
    reset();
  }
  function reset() {
    stop(); held = null; sel = null; results = null;
    ["results", "pax", "meals", "done"].forEach((k) => (S(k).innerHTML = ""));
    renderSearch(); step(1);
  }

  /* ---- step 1: search ---- */
  function renderSearch() {
    const opts = (cur) => M.stations.map((s) => `<option value="${s.code}"${s.code === cur ? " selected" : ""}>${esc(s.name)} (${s.code})</option>`).join("");
    S("search").innerHTML = `<div class="card"><h3>Search trains</h3>
      <div class="search">
        <div><label>From (starting station)</label><select id="s-from">${opts(q.from)}</select></div>
        <button class="swap" id="s-swap" title="Swap stations" type="button">&#8646;</button>
        <div><label>To (destination)</label><select id="s-to">${opts(q.to)}</select></div>
        <div><label>Date of journey</label><input type="date" id="s-date" min="${M.today}" max="${M.maxDate}" value="${q.date}"></div>
        <button class="primary" id="s-go" type="button">Search trains</button>
      </div><div id="s-err"></div></div>`;
    $("#s-swap", el).onclick = () => { const f = $("#s-from", el), t = $("#s-to", el), x = f.value; f.value = t.value; t.value = x; };
    $("#s-go", el).onclick = search;
  }
  async function search() {
    const err = $("#s-err", el);
    q = { from: $("#s-from", el).value, to: $("#s-to", el).value, date: $("#s-date", el).value };
    if (q.from === q.to) { err.innerHTML = `<div class="msg err">Starting station and destination must be different.</div>`; return; }
    if (!q.date) { err.innerHTML = `<div class="msg err">Please choose the date of journey.</div>`; return; }
    stop(); if (held) { post("/api/release", { ...base(), pnr: held.pnr }); held = null; }      /* new search frees the held seats */
    ["pax", "meals", "done"].forEach((k) => (S(k).innerHTML = ""));
    err.innerHTML = `<div class="msg">Searching trains&hellip;</div>`;
    const d = await api("/api/trains?" + new URLSearchParams(q));
    if (!d.ok) { err.innerHTML = `<div class="msg err">${esc(d.message)}</div>`; S("results").innerHTML = ""; return; }
    err.innerHTML = ""; results = d; renderResults(); step(2);
  }

  /* ---- step 2: choose train + class ---- */
  function renderResults() {
    const d = results;
    const card = (t) => `<div class="tcard${t.status === "DEPARTED" ? " off" : ""}">
      <div class="thead"><div><b>${t.no}</b> ${esc(t.name)}</div><span class="muted">${t.km} km &middot; ${t.duration}</span></div>
      <div class="times"><div><b>${hm(t.dep)}</b><small>${fmtDay(t.dep)}</small></div><div class="line"></div><div><b>${hm(t.arr)}</b><small>${fmtDay(t.arr)}</small></div>
        ${t.status === "DEPARTED" ? `<span class="badge CANCELLED">Departed</span>` : ""}</div>
      <div class="cls">${t.classes.map((c) => `<button type="button" class="clsbtn${c.free > 0 && c.free <= 10 ? " low" : ""}" data-t="${t.no}" data-c="${c.code}"${c.free < 1 || t.status === "DEPARTED" ? " disabled" : ""}>
        <b>${c.code}</b> &nbsp;${rs(c.fare)}${SITE.discount ? ` <small class="off">${SITE.discount}% off &rarr; ${rs(c.fare - Math.floor(c.fare * SITE.discount / 100))}</small>` : ""}<small>${c.free < 1 ? "Sold out" : c.free + " berths free"}</small></button>`).join("")}</div></div>`;
    S("results").innerHTML = `<div class="card"><h3>${d.trains.length} train${d.trains.length === 1 ? "" : "s"}: ${esc(d.from)} &rarr; ${esc(d.to)} &middot; ${fmtDay(q.date)}</h3>
      ${d.trains.length ? d.trains.map(card).join("") : `<div class="msg err">No train runs between these two stations. Try another pair (for example Chennai Central &rarr; New Delhi, Katpadi Jn &rarr; Bengaluru).</div>`}
      <p class="muted">Fares are for one passenger and depend on the distance between your two stations. Pick a class to continue.</p></div>`;
    S("results").querySelectorAll(".clsbtn").forEach((b) => (b.onclick = () => {
      const t = d.trains.find((x) => x.no === +b.dataset.t);
      choose(t, t.classes.find((c) => c.code === b.dataset.c));
    }));
  }

  /* ---- step 3: passengers + berth preference ---- */
  function choose(t, c) {
    sel = { t, c }; S("results").innerHTML = ""; S("meals").innerHTML = ""; S("done").innerHTML = "";
    const cl = CLASSES[c.code];
    S("pax").innerHTML = `<div class="card"><h3>Passenger details &amp; berth preference</h3>
      <div class="tsum"><span><b>${t.no} ${esc(t.name)}</b></span><span>${esc(results.from)} &rarr; ${esc(results.to)}</span><span>${fmtDT(t.dep)}</span><span>${esc(cl.name)}</span><span>${rs(c.fare)} per passenger</span></div>
      <div class="grid" style="margin-top:12px">
        <div><label>Berth preference (seat numbers are auto-allotted)</label><select id="p-pref"><option value="NP">No preference</option>${cl.prefs.map((p) => `<option value="${p[0]}">${p[1]}</option>`).join("")}</select></div>
        <div><label>Number of passengers (max 6)</label><select id="p-count">${[1, 2, 3, 4, 5, 6].map((n) => `<option>${n}</option>`).join("")}</select></div></div>
      <div class="names" id="p-names"></div><div class="fare" id="p-fare"></div>
      <button class="primary" id="p-go" type="button">Continue &ndash; show my seats &amp; meals</button>
      <button class="ghost" id="p-back" type="button">&larr; Change train</button><div id="p-out"></div></div>`;
    const cnt = $("#p-count", el), names = $("#p-names", el);
    function refresh() {
      const n = +cnt.value, old = [...names.querySelectorAll("input")].map((i) => i.value);
      names.innerHTML = Array.from({ length: n }, (_, i) => `<input placeholder="Passenger ${i + 1} name" value="${esc(old[i] || "")}" maxlength="30">`).join("");
      const total = c.fare * n, disc = Math.floor(total * SITE.discount / 100);
      $("#p-fare", el).innerHTML = `<span>Ticket fare: <b>${rs(total)}</b></span>` + (SITE.discount ? `<span>${esc(SITE.name)} discount (${SITE.discount}%): <b>-${rs(disc)}</b></span>` : "") + `<span>Ticket total: <b>${rs(total - disc)}</b> <small class="muted">(meals are added in the next step)</small></span>`;
    }
    cnt.onchange = refresh; refresh(); step(3);
    $("#p-back", el).onclick = () => { S("pax").innerHTML = ""; renderResults(); step(2); };
    $("#p-go", el).onclick = async () => {
      const out = $("#p-out", el), list = [...names.querySelectorAll("input")].map((i) => i.value.trim()).filter(Boolean);
      if (!list.length) { out.innerHTML = `<div class="msg err">Please enter passenger name(s).</div>`; return; }
      out.innerHTML = `<div class="msg">Reserving seats in the booking engine&hellip;</div>`;
      const d = await post("/api/book", { ...base(), train: t.no, from: q.from, to: q.to, date: q.date, class: c.code, pref: $("#p-pref", el).value, names: list.join(","), hold: 1 });
      if (!d.ok) { out.innerHTML = `<div class="msg err">${esc(d.message)}</div>`; return; }
      held = d.booking; held.synced = d.synced; S("pax").innerHTML = ""; renderMeals();
      window.dispatchEvent(new Event("railway-refresh"));
    };
  }

  /* ---- step 4: seats are known -> meal per seat (or skip) ---- */
  function renderMeals() {
    const b = held, n = b.count; mealPick = Array(n).fill(0); let left = b.expiresIn;
    const mopts = M.meals.map((m) => `<option value="${m.id}">${m.id ? esc(m.name) + " - " + rs(m.price) : "No meal (skip)"}</option>`).join("");
    const rows = b.seats.map((s, i) => `<tr><td><b>${esc(s.label)}</b><br><small class="muted">${esc(s.typeName)} berth</small></td><td>${esc(b.names[i])}</td>
      <td><select class="mealsel" data-i="${i}">${mopts}</select></td><td class="mprice" id="mp-${i}">${rs(0)}</td></tr>`).join("");
    S("meals").innerHTML = `<div class="card"><h3>Your seats &amp; meals</h3>
      <div class="tsum"><span><b>PNR ${b.pnr}</b> (seats held)</span><span>${b.trainNo} ${esc(b.trainName)}</span><span>${esc(b.fromCode)} &rarr; ${esc(b.toCode)}</span><span>${fmtDT(b.dep)}</span><span>${esc(b.className)}</span>
        <span class="timer">Seats held for <b id="m-timer"></b></span></div>
      <div class="note ${b.count === 1 ? "single" : b.together ? "together" : "split"}">${b.count > 1 && !b.together ? "&#9888; " : ""}${esc(b.note)}</div>
      <p class="muted">Meals are served at your seat. Choose a meal for each seat number, or leave &ldquo;No meal&rdquo; for anyone who does not want one.</p>
      ${b.count > 1 ? `<div style="max-width:300px;margin-bottom:10px"><label>Same meal for everyone (optional shortcut)</label><select id="m-all"><option value="">&mdash; choose &mdash;</option>${mopts}</select></div>` : ""}
      <div class="scroll"><table><tr><th>Seat</th><th>Passenger</th><th>Meal for this seat</th><th>Price</th></tr>${rows}</table></div>
      <div class="fare" id="m-fare"></div>
      <button class="primary" id="m-go" type="button"></button>
      <button class="ghost" id="m-skip" type="button">Skip meals &amp; book ticket</button>
      <button class="ghost" id="m-cancel" type="button">Cancel &amp; release seats</button><div id="m-out"></div></div>`;
    step(4);
    const tick = () => { const e = $("#m-timer", el); if (e) e.textContent = Math.floor(left / 60) + ":" + String(left % 60).padStart(2, "0"); };
    function totals() {
      const mt = mealPick.reduce((a, m) => a + M.meals[m].price, 0);
      b.seats.forEach((s, i) => ($("#mp-" + i, el).textContent = rs(M.meals[mealPick[i]].price)));
      $("#m-fare", el).innerHTML = `<span>Ticket fare: <b>${rs(b.fare)}</b></span>` + (b.discount ? `<span>Discount (${b.discountPct}%): <b>-${rs(b.discount)}</b></span>` : "") + `<span>Meals: <b>${rs(mt)}</b></span><span>You pay: <b>${rs(b.fare - b.discount + mt)}</b></span>`;
      $("#m-go", el).textContent = mt ? `Confirm ticket with meals (${rs(b.fare - b.discount + mt)})` : `Confirm ticket (no meals)`;
    }
    el.querySelectorAll(".mealsel").forEach((s) => (s.onchange = () => { mealPick[+s.dataset.i] = +s.value; totals(); }));
    if ($("#m-all", el)) $("#m-all", el).onchange = (e) => {
      if (e.target.value === "") return;
      mealPick = mealPick.map(() => +e.target.value);
      el.querySelectorAll(".mealsel").forEach((s) => (s.value = e.target.value)); totals();
    };
    totals(); tick(); stop();
    timer = setInterval(() => {
      left--; tick();
      if (left <= 0) { stop(); held = null; S("meals").innerHTML = `<div class="card"><div class="msg err">Seat hold expired - the seats went back to the pool. Please search again.</div><button class="primary" id="m-again" type="button">Search again</button></div>`; $("#m-again", el).onclick = reset; window.dispatchEvent(new Event("railway-refresh")); }
    }, 1000);

    async function confirm(meals) {
      const out = $("#m-out", el); out.innerHTML = `<div class="msg">Confirming with the booking engine&hellip;</div>`;
      const d = await post("/api/confirm", { ...base(), pnr: b.pnr, meals: meals.join(",") });
      if (!d.ok) { out.innerHTML = `<div class="msg err">${esc(d.message)}</div>`; if (/expired/i.test(d.message)) { stop(); held = null; } return; }
      stop(); held = null; S("meals").innerHTML = "";
      S("done").innerHTML = `<div class="msg ok">${esc(d.message)}${b.synced ? `<span class="sync">&#10003; Synced to Railway Portal database</span>` : ""}</div>` + ticketHTML(d.booking) + `<button class="primary" id="d-new" type="button">Book another ticket</button>`;
      $("#d-new", el).onclick = reset; step(5);
      window.dispatchEvent(new Event("railway-refresh"));
    }
    $("#m-go", el).onclick = () => confirm(mealPick);
    $("#m-skip", el).onclick = () => confirm(Array(n).fill(0));
    $("#m-cancel", el).onclick = async () => { await post("/api/release", { ...base(), pnr: b.pnr }); reset(); window.dispatchEvent(new Event("railway-refresh")); };
  }
  init();
}

/* ---------------- cancellation ---------------- */
function mountCancel(el) {
  el.innerHTML = `<div class="grid"><div class="card"><h3>Cancel ticket &amp; refund</h3>
    <label>PNR number</label><input id="cn-pnr" placeholder="e.g. 1000001" inputmode="numeric">
    <label style="margin-top:10px">Cancelling how long before departure? (simulation)</label>
    <select id="cn-hrs"><option value="">Automatic (use real time to departure)</option><option value="72">More than 48 hours</option><option value="24">12 to 48 hours</option><option value="8">4 to 12 hours</option><option value="2">Less than 4 hours</option></select>
    <button class="primary" id="cn-go">Cancel ticket</button><div id="cn-out"></div></div>
    <div class="card"><h3>Refund policy</h3><table class="policy"><tr><th>Before departure</th><th>Refund</th></tr>
    <tr><td>48 hours or more</td><td>Paid amount minus flat fee per passenger (1AC ${RUPEE}240, 2AC ${RUPEE}200, 3AC ${RUPEE}180)</td></tr>
    <tr><td>12 to 48 hours</td><td>75% of paid amount</td></tr><tr><td>4 to 12 hours</td><td>50% of paid amount</td></tr>
    <tr><td>Less than 4 hours</td><td>No refund</td></tr></table>
    <p class="muted">Meals ordered with the ticket are refunded in full when you cancel 4 hours or more before departure; under 4 hours the pantry is already loaded and meals are not refunded.</p><p class="muted">${SITE.channel === "DIRECT" ? "The Railway Portal can cancel any ticket." : "You can cancel only tickets booked through " + esc(SITE.name) + "."}</p></div></div>`;
  $("#cn-go", el).onclick = async () => {
    const out = $("#cn-out", el), pnr = $("#cn-pnr", el).value.trim();
    const d = await post("/api/cancel", { channel: SITE.channel, key: SITE.key, pnr, hours: $("#cn-hrs", el).value });
    out.innerHTML = d.ok ? `<div class="msg ok">${esc(d.message)}</div>` + ticketHTML(d.booking) : `<div class="msg err">${esc(d.message)}</div>`;
    window.dispatchEvent(new Event("railway-refresh"));
  };
}

/* ---------------- bookings table (reads the MAIN database) ---------------- */
function mountBookings(el, fixedChannel) {
  el.innerHTML = `<div class="card"><h3>${fixedChannel ? "My bookings (read live from the Railway Portal database)" : "All bookings (direct + partner websites)"}</h3>
    <div id="bl-stats" class="stats"></div>
    ${fixedChannel ? "" : `<div style="max-width:260px;margin-bottom:10px"><label>Filter by booking source</label><select id="bl-filter"><option value="">All sources</option><option value="DIRECT">Railway Portal</option><option value="MMT">MakeMyTrip</option><option value="GOI">Goibibo</option></select></div>`}
    <div class="scroll"><table><thead><tr><th>PNR</th><th>Source</th><th>Priority</th><th>Train &amp; journey</th><th>Class</th><th>Pax</th><th>Seats</th><th>Meals</th><th>Together?</th><th>Paid</th><th>Status</th><th>Booked at</th></tr></thead><tbody id="bl-body"></tbody></table></div></div>`;
  const seen = new Set(); let first = true;
  async function load() {
    const ch = fixedChannel || ($("#bl-filter", el) ? $("#bl-filter", el).value : "");
    const d = await api("/api/bookings" + (ch ? "?channel=" + ch : ""));
    if (!d.bookings) return;
    $("#bl-body", el).innerHTML = d.bookings.slice().reverse().map((b) => {
      const isNew = !first && !seen.has(b.pnr); seen.add(b.pnr);
      const meals = b.seats.filter((s) => s.mealId).length;
      return `<tr class="${isNew ? "new" : ""}"><td><b>${b.pnr}</b></td><td><span class="badge ${b.channel}">${esc(b.channelName)}</span></td><td><span class="badge p${b.priority}">${b.priority === 1 ? "High (partner)" : "Normal"}</span></td>
      <td><b>${b.trainNo}</b> ${esc(b.trainName)}<br><small>${esc(b.fromCode)} &rarr; ${esc(b.toCode)} &middot; ${fmtDT(b.dep)}</small></td>
      <td>${b.class}</td><td>${b.count}</td><td>${b.seats.map((s) => esc(s.label)).join(", ")}</td><td>${meals ? `${meals} <small class="muted">(${rs(b.mealTotal)})</small>` : "-"}</td><td>${b.count === 1 ? "-" : b.together ? "Yes" : "<b style='color:#b26a00'>NO</b>"}</td>
      <td>${rs(b.paid)}${b.discount ? `<br><small class="muted">saved ${rs(b.discount)}</small>` : ""}</td><td><span class="badge ${b.status}">${b.status}</span>${b.status === "CANCELLED" ? `<br><small>refund ${rs(b.refund)}</small>` : ""}</td><td>${esc(b.bookedAt)}</td></tr>`;
    }).join("") || `<tr><td colspan="12" class="muted">No bookings yet.</td></tr>`;
    first = false;
    const s = await api("/api/stats");
    if (s.byChannel) $("#bl-stats", el).innerHTML =
      `<div class="stat"><b>${s.confirmed}</b><span>tickets confirmed</span></div><div class="stat"><b>${s.byChannel.DIRECT}</b><span>Railway Portal</span></div><div class="stat"><b>${s.byChannel.MMT}</b><span>MakeMyTrip</span></div><div class="stat"><b>${s.byChannel.GOI}</b><span>Goibibo</span></div><div class="stat"><b>${s.cancelled}</b><span>cancelled</span></div><div class="stat"><b>${rs(s.refunded)}</b><span>refunded</span></div><div class="stat"><b>${s.meals}</b><span>meals ordered (${rs(s.mealRevenue)})</span></div><div class="stat"><b>${s.held}</b><span>seats on hold now</span></div><div class="stat"><b>${s.audit.correct ? "OK" : "ERROR"}</b><span>seat audit (${s.audit.doubleBooked} double)</span></div>`;
  }
  if ($("#bl-filter", el)) $("#bl-filter", el).onchange = () => { first = true; load(); };
  load(); setInterval(load, 2000); window.addEventListener("railway-refresh", load);
}

/* ---------------- train + date picker used by the availability and pantry tabs ---------------- */
function trainDatePicker(el, title, intro, onLoad) {
  el.innerHTML = `<div class="card"><h3>${title}</h3><p class="muted">${intro}</p>
    <div class="grid"><div><label>Train</label><select id="tp-train"></select></div>
    <div><label>Date (departure from the train's first station)</label><input type="date" id="tp-date"></div></div>
    <div id="tp-out" style="margin-top:14px"></div></div>`;
  const out = $("#tp-out", el);
  async function load() {
    const t = $("#tp-train", el).value, d = $("#tp-date", el).value;
    if (!t || !d) return;
    const r = await onLoad(t, d); if (r !== undefined) out.innerHTML = r;
  }
  getMeta().then((M) => {
    if (!M.trains) return;
    $("#tp-train", el).innerHTML = M.trains.map((t) => `<option value="${t.no}">${t.no} ${esc(t.name)} (${esc(t.route)})</option>`).join("");
    $("#tp-date", el).min = M.today; $("#tp-date", el).value = addDays(M.today, 1);
    load();
  });
  $("#tp-train", el).onchange = $("#tp-date", el).onchange = load;
  setInterval(load, 3000); window.addEventListener("railway-refresh", load);
}

/* ---------------- availability ---------------- */
function mountAvailability(el) {
  trainDatePicker(el, "Seat availability", "Free berths per class and berth type for one train on one date. Every train and date has its own berth map.", async (t, d) => {
    const r = await api("/api/availability?" + new URLSearchParams({ train: t, date: d }));
    if (!r.classes) return `<div class="msg err">${esc(r.message || "Cannot load")}</div>`;
    return `<div class="avail">` + r.classes.map((c) => `<div class="card" style="margin:0"><b>${esc(c.name)}</b><div class="big">${c.free}<small class="muted"> / ${c.total} free</small></div><div class="muted">Fare ${RUPEE}${(c.rate / 100).toFixed(2)} per km per passenger (min fare applies)</div><div class="chips">` +
      Object.entries(c.types).map(([k, v]) => `<span class="chip">${esc(v.name)}: <b>${v.free}</b></span>`).join("") + `</div></div>`).join("") + `</div>`;
  });
}

/* ---------------- pantry / meal manifest (which meal goes to which seat) ---------------- */
function mountMeals(el) {
  trainDatePicker(el, "Pantry car - meal manifest", "Meals ordered by passengers, sorted by seat number, so the pantry staff know exactly where to deliver each one.", async (t, d) => {
    const r = await api("/api/meals?" + new URLSearchParams({ train: t, date: d }));
    if (!r.rows) return `<div class="msg err">${esc(r.message || "Cannot load")}</div>`;
    return `<div class="stats">` + r.summary.map((s) => `<div class="stat"><b>${s.count}</b><span>${esc(s.meal)}</span></div>`).join("") + `</div>` +
      `<div class="scroll"><table><tr><th>Seat</th><th>Class</th><th>Passenger</th><th>Meal</th><th>Price</th><th>PNR</th><th>Journey</th></tr>` +
      (r.rows.map((x) => `<tr><td><b>${esc(x.seat)}</b></td><td>${x.class}</td><td>${esc(x.name)}</td><td>${esc(x.meal)}</td><td>${rs(x.price)}</td><td>${x.pnr}</td><td>${esc(x.route)}</td></tr>`).join("") || `<tr><td colspan="7" class="muted">No meals ordered for this train and date yet.</td></tr>`) + `</table></div>`;
  });
}

/* ---------------- activity log ---------------- */
function mountLog(el) {
  el.innerHTML = `<div class="card"><h3>Booking-engine activity log</h3><p class="muted">Shows which worker thread served which request, in the order served (priority scheduling).</p><pre class="log" id="lg"></pre></div>`;
  async function load() { const d = await api("/api/log"); if (!d.log) return; const p = $("#lg", el); const atEnd = p.scrollTop + p.clientHeight >= p.scrollHeight - 30; p.textContent = d.log.join("\n"); if (atEnd) p.scrollTop = p.scrollHeight; }
  load(); setInterval(load, 1500); window.addEventListener("railway-refresh", load);
}

/* ---------------- demo / admin (main site only) ---------------- */
function mountDemo(el) {
  el.innerHTML = `<div class="card"><h3>Priority demo</h3><p>Sends <b>6 direct</b> and <b>6 partner</b> (MakeMyTrip + Goibibo) booking requests at the same moment, all for the same train and date (12345 Chennai Express, Chennai Central to New Delhi, 7 days from today). Workers are held until all requests are queued, so you can see that partner bookings are served first. To avoid starving direct customers, after every 3 partner requests one waiting direct request is served (aging).</p>
    <button class="primary" id="dm-burst">Run priority demo</button> <button class="ghost" id="dm-reset">Reset all bookings</button><div id="dm-out"></div></div>`;
  $("#dm-burst", el).onclick = async () => {
    $("#dm-out", el).innerHTML = `<div class="msg">Running&hellip;</div>`;
    const d = await post("/api/demo/burst", {});
    if (!d.ok) { $("#dm-out", el).innerHTML = `<div class="msg err">${esc(d.message || "failed")}</div>`; return; }
    const o = d.order.slice().sort((a, b) => a.rank - b.rank);
    $("#dm-out", el).innerHTML = `<div class="msg ok">Average service position: partner <b>${d.avgRankPartner.toFixed(1)}</b> vs direct <b>${d.avgRankDirect.toFixed(1)}</b> (lower = served earlier)</div>
      <div class="scroll"><table><tr><th>Served #</th>${o.map((x) => `<td><span class="badge ${x.channel}">${x.channel}</span></td>`).join("")}</tr></table></div>`;
    window.dispatchEvent(new Event("railway-refresh"));
  };
  $("#dm-reset", el).onclick = async () => { await post("/api/reset", {}); $("#dm-out", el).innerHTML = `<div class="msg ok">All berths are free again.</div>`; window.dispatchEvent(new Event("railway-refresh")); };
}
