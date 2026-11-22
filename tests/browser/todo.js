// The example app (examples/web/todo.lube), as someone would use it.
(async () => {
  const out = {};
  const snap = () => ({
    items: $$('li').map(li => li.textContent.replace('×', '').trim() + (li.className ? ' [' + li.className + ']' : '')),
    left: $('footer span').textContent,
    clearDisabled: $('footer button').disabled,
  });
  out.styled = getComputedStyle(document.body).backgroundColor !== 'rgba(0, 0, 0, 0)';  // (/assets/app.css came)
  out.start = snap();
  // Adding: the form clears after each, and markup in what is typed stays text.
  for (const t of ['milk', 'eggs', 'bread', '<b>bold</b>']) {
    $('form input').value = t;
    $('form').requestSubmit();
    await settled();
  }
  out.added = { ...snap(), formCleared: $('form input').value === '', bold: $$('li b').length };
  // Typing fast in the filter: it keeps its focus, its caret and every letter, as views come back.
  const f = $('.filter');
  f.focus();
  for (const ch of 'brea') {
    f.value += ch;
    f.setSelectionRange(f.value.length, f.value.length);
    f.dispatchEvent(new Event('input', { bubbles: true }));
    await wait(15);
  }
  await settled();
  out.filtered = { ...snap(), sameField: f === $('.filter'), focused: document.activeElement === f, value: f.value, caret: f.selectionStart };
  f.value = '';
  f.dispatchEvent(new Event('input', { bubbles: true }));
  await settled();
  f.blur();
  // A checkbox: its item is the same element after, with the class its view gives it.
  const milk = $$('li').find(li => li.textContent.includes('milk'));
  milk.querySelector('input').click();
  await settled();
  out.checked = { ...snap(), checkbox: milk.querySelector('input').checked, sameElement: milk === $$('li')[0] };
  $$('li').find(li => li.textContent.includes('bread')).querySelector('.remove').click();
  await settled();
  out.removed = snap();
  $('footer button').click();
  await settled();
  out.clearedDone = snap();
  return out;
})()
