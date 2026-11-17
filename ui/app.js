'use strict';

const element = id => document.getElementById(id);
const messages = {
  invalid: 'Ключ не найден или данные не прошли проверку.',
  expired: 'Срок действия истёк.',
  device_mismatch: 'Ключ привязан к другому тестовому устройству.',
};
let ticket = null;

async function request(action, body) {
  const response = await fetch(`/api/${action}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  return response.json();
}

function log(text) {
  const item = document.createElement('li');
  const time = document.createElement('time');
  const label = document.createElement('span');
  time.textContent = new Date().toLocaleTimeString('ru-RU');
  label.textContent = text;
  item.append(time, label);
  const activity = element('activity');
  activity.prepend(item);
  if (activity.children.length > 5) activity.lastElementChild.remove();
}

function session(result) {
  ticket = result.ticket;
  element('sign-screen').hidden = true;
  element('dashboard').hidden = false;
  element('session-status').textContent = 'Действительна до ' + new Date(result.expiresAt * 1000).toLocaleTimeString('ru-RU');
}

element('auth-form').addEventListener('submit', async event => {
  event.preventDefault();
  const button = event.submitter;
  if (button) button.disabled = true;
  const notice = element('notice');
  notice.textContent = '';
  try {
    const result = await request('auth', { key: element('key-input').value.trim(), device: 'a'.repeat(64) });
    if (result.status !== 'ok') notice.textContent = messages[result.status] ?? 'Не удалось проверить ключ.';
    else { session(result); log('Тестовый ключ активирован. Сервер подписал билет Ed25519.'); }
  } catch { notice.textContent = 'Локальный API недоступен.'; }
  finally { if (button) button.disabled = false; }
});

element('refresh').addEventListener('click', async event => {
  const button = event.currentTarget;
  const pendingTicket = ticket;
  button.disabled = true;
  try {
    const result = await request('refresh', { ticket: pendingTicket });
    if (ticket !== pendingTicket) return;
    if (result.status === 'ok') { session(result); log('Подпись проверена. Сессия продлена.'); }
    else log(messages[result.status] ?? 'Не удалось продлить сессию.');
  } catch { if (ticket === pendingTicket) log('Локальный API недоступен.'); }
  finally { button.disabled = false; }
});

element('signout').addEventListener('click', () => {
  ticket = null;
  element('activity').replaceChildren();
  element('dashboard').hidden = true;
  element('sign-screen').hidden = false;
});
