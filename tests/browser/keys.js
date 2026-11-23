// tests/browser/keys.lube: list items keep their elements as they move; fields keep what is typed in them.
(async () => {
  const out = {};
  const order = () => $$('li').map(li => li.firstChild.textContent + '=' + li._was);
  $$('li').forEach(li => li._was = li.firstChild.textContent);  // (an element's own mark: it stays with the element)
  const c = $$('li')[2].querySelector('input');
  c.focus();
  c.value = 'typed in c';
  $('#flip').click();
  await settled();
  out.flipped = order();
  out.focusKept = document.activeElement === c && c.value === 'typed in c';
  $('#rotate').click();
  await settled();
  out.rotated = order();
  out.stillFocused = document.activeElement === c;
  // A field the server does not set keeps what was typed in it, after it loses focus too.
  const d = $$('li')[0].querySelector('input');
  d.value = 'mine';
  d.blur();
  out.textareaBefore = $('#t').value;
  $('#more').click();
  await settled();
  out.unownedKept = d.value;
  out.textareaAfter = $('#t').value;
  out.select = $('#s').value;
  out.extra = $('p').textContent;
  // A handler that fails: the page says so on its console, keeps its view, and goes on working.
  $('#fail').click();
  await settled();
  out.afterFail = $('p').textContent;
  $('#more').click();
  await settled();
  out.afterMore = $('p').textContent;
  return out;
})()
