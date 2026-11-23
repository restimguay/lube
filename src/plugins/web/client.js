(function () {
  'use strict';
  var root = document.getElementById('lube');
  if (!root || !root.getAttribute('data-page')) return;
  var version = +root.getAttribute('data-version');
  var url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/_lube/ws?page=' + root.getAttribute('data-page');
  var ws = new WebSocket(url);
  var EVENTS = ['click', 'dblclick', 'input', 'change', 'submit', 'keydown', 'keyup', 'focus', 'blur', 'mouseenter', 'mouseleave'];
  var OWN = {focus: 1, blur: 1, mouseenter: 1, mouseleave: 1};  // events that do not bubble: the element's own

  // Events go out one at a time, at most one every 60 ms (the server allows 20 a second): an input's latest value
  // replaces one still waiting, so typing fast sends what was typed, not every key.
  var queue = [], timer = 0, sent = 0, replies = 0, submitAt = 0, submitted = null, forced = null;
  // data-busy is on the root while an event waits to go or for its answer (for CSS, and for tests: once it is
  // gone, the page shows what the server last said).
  function busy() {
    if (queue.length || sent > replies) root.setAttribute('data-busy', '');
    else root.removeAttribute('data-busy');
  }
  function send(m) {
    if (m.e === 'input') {
      for (var i = 0; i < queue.length; i++) if (queue[i].e === 'input' && queue[i].h === m.h) { queue[i] = m; return; }
    }
    queue.push(m);
    busy();
    if (!timer) flush();
  }
  function flush() {
    timer = 0;
    if (!queue.length || ws.readyState !== 1) return;
    var m = queue.shift(), out = {v: version, h: m.h};
    if (m.a !== undefined) out.a = m.a;
    ws.send(JSON.stringify(out));
    sent++;
    if (m.form) { submitAt = sent; submitted = m.form; }  // its answer sets the form's fields, even the one with focus
    busy();
    timer = setTimeout(flush, 60);
  }
  ws.onopen = flush;

  EVENTS.forEach(function (e) {
    root.addEventListener(e, function (ev) {
      var t = ev.target, attr = 'data-on-' + e;
      if (!OWN[e]) t = t.closest ? t.closest('[' + attr + ']') : null;
      if (!t || !t.getAttribute || !t.getAttribute(attr) || !root.contains(t)) return;
      var m = {e: e, h: t.getAttribute(attr)};
      if (e === 'submit') {
        ev.preventDefault();
        m.form = t;
        m.a = {};
        new FormData(t).forEach(function (v, k) { if (typeof v === 'string') m.a[k] = v; });
      } else if (e === 'input' || e === 'change') {
        m.a = t.type === 'checkbox' || t.type === 'radio' ? t.checked : t.value;
      } else if (e === 'keydown' || e === 'keyup') {
        m.a = ev.key;
      } else if (e === 'click' && t.tagName === 'A') {
        ev.preventDefault();
      }
      send(m);
    }, !!OWN[e]);
  });

  // The new view, made to match the page's in place: what is the same stays (focus, the caret, what is typed in a
  // field that has focus), and only what differs changes. data-key="..." (key: in a view) matches a list's items.
  function key(n) { return n.nodeType === 1 ? n.getAttribute('data-key') : null; }
  function same(a, b) {
    return a.nodeType === b.nodeType && (a.nodeType !== 1 || (a.tagName === b.tagName && key(a) === key(b)));
  }
  function attrs(a, b) {
    for (var i = a.attributes.length - 1; i >= 0; i--) {
      var n = a.attributes[i].name;
      if (!b.hasAttribute(n)) a.removeAttribute(n);
    }
    for (var j = 0; j < b.attributes.length; j++) {
      var at = b.attributes[j];
      if (a.getAttribute(at.name) !== at.value) a.setAttribute(at.name, at.value);
    }
  }
  // A field's value: what the server says, when that changed (or the form was just sent), and not while it has
  // focus: what someone is typing stays theirs.
  function selected(s) {
    var k = '';
    for (var i = 0; i < s.options.length; i++) k += s.options[i].hasAttribute('selected') ? '1' : '0';
    return k;
  }
  function node(a, b) {
    if (a.nodeType !== 1) {
      if (a.nodeValue !== b.nodeValue) a.nodeValue = b.nodeValue;
      return;
    }
    var tag = a.tagName, force = forced !== null && forced.contains(a);
    var set = force || a !== document.activeElement;
    var was = tag === 'TEXTAREA' ? a.textContent : tag === 'SELECT' ? selected(a) : a.getAttribute('value');
    attrs(a, b);
    if (tag === 'INPUT') {
      if (a.type === 'checkbox' || a.type === 'radio') a.checked = b.hasAttribute('checked');
      else if (set && (force || was !== b.getAttribute('value'))) a.value = b.getAttribute('value') || '';
      return;
    }
    if (tag === 'TEXTAREA') {
      if (was !== b.textContent) a.textContent = b.textContent;
      if (set && (force || was !== b.textContent)) a.value = b.textContent;
      return;
    }
    children(a, b);
    if (tag === 'SELECT' && set && (force || was !== selected(a)))
      for (var i = 0; i < a.options.length; i++) a.options[i].selected = a.options[i].hasAttribute('selected');
  }
  function children(p, q) {
    var a = p.firstChild, b = q.firstChild;
    while (b) {
      var next = b.nextSibling;
      if (a && same(a, b)) {
        node(a, b);
        a = a.nextSibling;
      } else {
        var k = key(b), m = null;
        if (k !== null) for (var x = a; x; x = x.nextSibling) if (key(x) === k && same(x, b)) { m = x; break; }
        if (m) {
          p.insertBefore(m, a);  // (a list's item, moved)
          node(m, b);
        } else {
          p.insertBefore(b, a);  // (b leaves the new view: next was kept)
        }
      }
      b = next;
    }
    while (a) {
      var gone = a;
      a = a.nextSibling;
      p.removeChild(gone);
    }
  }

  // Each event gets one answer: {v} (and html: the new view; error: what went wrong).
  ws.onmessage = function (msg) {
    var d = JSON.parse(msg.data);
    replies++;
    if (submitAt && replies >= submitAt) { forced = submitted; submitAt = 0; submitted = null; }
    if (typeof d.html === 'string') {
      version = d.v;
      var t = document.createElement('template'), f = document.activeElement, from = null, to = null;
      try { from = f.selectionStart; to = f.selectionEnd; } catch (e) {}
      t.innerHTML = d.html;
      children(root, t.content);
      if (f && f !== document.activeElement && root.contains(f)) {  // (moved, it lost its focus: it gets it back)
        f.focus();
        try { if (from !== null) f.setSelectionRange(from, to); } catch (e) {}
      }
    }
    forced = null;
    busy();
    if (d.error) console.error('lube:', d.error);
  };
  ws.onclose = function () { root.setAttribute('data-closed', ''); };
})();
