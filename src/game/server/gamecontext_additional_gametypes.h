#ifdef CONTEXT_INIT_WITHOUT_CONFIG
if (str_comp(m_Config->m_SvGametype, "SAH") == 0 || str_comp(m_Config->m_SvGametype, "sah") == 0)
	m_pController = new CGameControllerSAH(this);
else
#else
if (str_comp(m_Config->m_SvGametype, "SAH") == 0 || str_comp(m_Config->m_SvGametype, "sah") == 0)
	m_pController = new CGameControllerSAH(this, *pConfig);
else
#endif