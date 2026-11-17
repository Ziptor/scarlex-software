import express from 'express';
import { fileURLToPath } from 'node:url';
import { createLicenseDemo } from './licenses.mjs';

const app = express();
const demo = createLicenseDemo();
app.disable('x-powered-by');
app.use(express.json({ limit: '4kb' }));
app.use((_request, response, next) => {
  response.set('Cache-Control', 'no-store');
  next();
});
const api = (method, request, response) => {
  if (!request.is('application/json')) return response.status(415).json({ status: 'invalid' });
  const result = method(request.body ?? {});
  return response.status(result.status === 'ok' ? 200 : 403).json(result);
};
app.get('/api/health', (_request, response) => response.json({ status: 'ready', mode: 'demo' }));
app.post('/api/auth', (request, response) => api(demo.activate, request, response));
app.post('/api/refresh', (request, response) => api(demo.refresh, request, response));
app.use(express.static(fileURLToPath(new URL('./ui/', import.meta.url))));
app.use((error, _request, response, _next) => {
  response.status(error.status === 413 ? 413 : error.status === 400 ? 400 : 500).json({ status: 'error' });
});
const server = app.listen(4300, '127.0.0.1', () => console.log('Scarlex demo: http://127.0.0.1:4300'));
server.on('close', demo.close);
