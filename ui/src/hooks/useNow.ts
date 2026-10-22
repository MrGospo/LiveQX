import React from 'react';

// The current time in milliseconds, refreshed on a timer, for pages that show
// "3 minutes ago". The page is re-drawn by the clock alone: nothing is asked of
// the server, so this is not polling. Stops while the tab is hidden.
export function useNow(everyMs = 30_000): number {
  const [now, setNow] = React.useState(() => Date.now());
  React.useEffect(() => {
    const tick = () => { if (!document.hidden) setNow(Date.now()); };
    const id = setInterval(tick, everyMs);
    document.addEventListener('visibilitychange', tick);     // catch up at once when the tab comes back
    return () => { clearInterval(id); document.removeEventListener('visibilitychange', tick); };
  }, [everyMs]);
  return now;
}
