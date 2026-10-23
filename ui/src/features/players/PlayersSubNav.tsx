import { usePlayerTree } from '@/api/queries/players';
import { SubNav } from '@/components/SubNav';
import { useAuthStore } from '@/stores/auth';
import { PLAYERS_NAV } from './nav';

// The tabs of the screens area. The journal tab is shown to an administrator, and to anyone
// who has been given the journal of at least one place; the others never see it (the server
// refuses them anyway, this only spares them a dead tab).
export function PlayersSubNav() {
  const role = useAuthStore(s => s.role);
  const tree = usePlayerTree();
  const mayRead = role === 'admin'
    || (tree.data ?? []).some(o => o.can?.view_audit === true || o.groups.some(g => g.can?.view_audit === true));
  return <SubNav items={PLAYERS_NAV.filter(i => i.to !== '/players/audit' || mayRead)} />;
}
