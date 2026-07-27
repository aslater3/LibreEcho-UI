/* Distinguish the built-in voice path from the configurable local model. */
(function () {
  function relabel() {
    document.querySelectorAll('details').forEach(details => {
      if (details.textContent.includes('Provider setup will appear here')) {
        const heading = details.querySelector('summary h3');
        if (heading) heading.textContent = 'On Device Voice Assistant';
      }
    });
  }
  new MutationObserver(relabel).observe(document.body, {childList:true, subtree:true});
  relabel();
})();
