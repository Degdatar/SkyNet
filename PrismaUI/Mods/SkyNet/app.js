const urlBar = document.getElementById('url-bar');
const viewport = document.getElementById('web-viewport');
const status = document.getElementById('browser-status');

function setStatus(message, isError = false) {
  status.textContent = message;
  status.classList.toggle('error', isError);
}

function sendToPlugin(functionName, argument = '') {
  const callback = window[functionName];
  if (typeof callback === 'function') {
    callback(argument);
  }
}

function navigateTo(url) {
  let targetUrl = url.trim();
  if (!targetUrl) return;

  if (!/^https?:\/\//i.test(targetUrl)) {
    if (!targetUrl.includes('.')) {
      targetUrl = `https://www.google.com/search?q=${encodeURIComponent(targetUrl)}`;
    } else {
      targetUrl = `https://${targetUrl}`;
    }
  }

  urlBar.value = targetUrl;
  setStatus(`Opening ${targetUrl}`);

  // Remote sites are intentionally not allowed to render in an iframe by
  // their X-Frame-Options/CSP policies. Navigate the PrismaUI view itself so
  // the page is a top-level document instead of an embedded frame.
  window.location.assign(targetUrl);
}

document.getElementById('btn-go').addEventListener('click', () => navigateTo(urlBar.value));
urlBar.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') navigateTo(urlBar.value);
});

document.getElementById('btn-back').addEventListener('click', () => {
  try {
    viewport.contentWindow.history.back();
  } catch {
    setStatus('Back is unavailable for this page.', true);
  }
});

document.getElementById('btn-forward').addEventListener('click', () => {
  try {
    viewport.contentWindow.history.forward();
  } catch {
    setStatus('Forward is unavailable for this page.', true);
  }
});

document.getElementById('btn-reload').addEventListener('click', () => {
  const url = viewport.src;
  if (url && url !== 'about:blank') {
    setStatus(`Reloading ${url}`);
    viewport.src = url;
  }
});

viewport.addEventListener('load', () => {
  setStatus('Page loaded. Some sites block embedded browsing and may appear blank.');
});

document.getElementById('btn-close').addEventListener('click', () => sendToPlugin('closeBrowser'));
